//===-- lib/MC/XCOFFObjectWriter.cpp - XCOFF file writer ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements XCOFF object file writer information.
//
//===----------------------------------------------------------------------===//

#include "llvm/BinaryFormat/XCOFF.h"
#include "llvm/MC/MCAsmLayout.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSectionXCOFF.h"
#include "llvm/MC/MCSymbolXCOFF.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/MCXCOFFObjectWriter.h"
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MathExtras.h"

#include <deque>

using namespace llvm;

// An XCOFF object file has a limited set of predefined sections. The most
// important ones for us (right now) are:
// .text --> contains program code and read-only data.
// .data --> contains initialized data, function descriptors, and the TOC.
// .bss  --> contains uninitialized data.
// Each of these sections is composed of 'Control Sections'. A Control Section
// is more commonly referred to as a csect. A csect is an indivisible unit of
// code or data, and acts as a container for symbols. A csect is mapped
// into a section based on its storage-mapping class, with the exception of
// XMC_RW which gets mapped to either .data or .bss based on whether it's
// explicitly initialized or not.
//
// We don't represent the sections in the MC layer as there is nothing
// interesting about them at at that level: they carry information that is
// only relevant to the ObjectWriter, so we materialize them in this class.
namespace {

constexpr unsigned DefaultSectionAlign = 4;

// Packs the csect's alignment and type into a byte.
uint8_t getEncodedType(const MCSectionXCOFF *);

// Wrapper around an MCSymbolXCOFF.
struct Symbol {
  const MCSymbolXCOFF *const MCSym;
  uint32_t SymbolTableIndex;

  XCOFF::StorageClass getStorageClass() const {
    return MCSym->getStorageClass();
  }
  StringRef getName() const { return MCSym->getName(); }
  bool nameInStringTable() const {
    return MCSym->getName().size() > XCOFF::NameSize;
  }

  Symbol(const MCSymbolXCOFF *MCSym) : MCSym(MCSym), SymbolTableIndex(-1) {}
};

// Wrapper for an MCSectionXCOFF.
struct ControlSection {
  const MCSectionXCOFF *const MCCsect;
  uint32_t SymbolTableIndex;
  uint32_t Address;
  uint32_t Size;

  SmallVector<Symbol, 1> Syms;

  ControlSection(const MCSectionXCOFF *MCSec)
      : MCCsect(MCSec), SymbolTableIndex(-1), Address(-1) {}
};

// Represents the data related to a section excluding the csects that make up
// the raw data of the section. The csects are stored separately as not all
// sections contain csects, and some sections contain csects which are better
// stored separately, e.g. the .data section containing read-write, descriptor,
// TOCBase and TOC-entry csects.
struct Section {
  char Name[XCOFF::NameSize];
  // The physical/virtual address of the section. For an object file
  // these values are equivalent.
  uint32_t Address;
  uint32_t Size;
  uint32_t FileOffsetToData;
  uint32_t FileOffsetToRelocations;
  uint32_t RelocationCount;
  int32_t Flags;

  uint16_t Index;

  // Virtual sections do not need storage allocated in the object file.
  const bool IsVirtual;

  void reset() {
    Address = 0;
    Size = 0;
    FileOffsetToData = 0;
    FileOffsetToRelocations = 0;
    RelocationCount = 0;
    Index = -1;
  }

  Section(const char *N, XCOFF::SectionTypeFlags Flags, bool IsVirtual)
      : Address(0), Size(0), FileOffsetToData(0), FileOffsetToRelocations(0),
        RelocationCount(0), Flags(Flags), Index(-1), IsVirtual(IsVirtual) {
    strncpy(Name, N, XCOFF::NameSize);
  }
};

class XCOFFObjectWriter : public MCObjectWriter {
  // Type to be used for a container representing a set of csects with
  // (approximately) the same storage mapping class. For example all the csects
  // with a storage mapping class of `xmc_pr` will get placed into the same
  // container.
  using ControlSections = std::deque<ControlSection>;

  support::endian::Writer W;
  std::unique_ptr<MCXCOFFObjectTargetWriter> TargetObjectWriter;
  StringTableBuilder Strings;

  // The non-empty sections, in the order they will appear in the section header
  // table.
  std::vector<Section *> Sections;

  // The Predefined sections.
  Section Text;
  Section BSS;

  // ControlSections. These store the csects which make up different parts of
  // the sections. Should have one for each set of csects that get mapped into
  // the same section and get handled in a 'similar' way.
  ControlSections ProgramCodeCsects;
  ControlSections BSSCsects;

  uint32_t SymbolTableEntryCount = 0;
  uint32_t SymbolTableOffset = 0;

  virtual void reset() override;

  void executePostLayoutBinding(MCAssembler &, const MCAsmLayout &) override;

  void recordRelocation(MCAssembler &, const MCAsmLayout &, const MCFragment *,
                        const MCFixup &, MCValue, uint64_t &) override;

  uint64_t writeObject(MCAssembler &, const MCAsmLayout &) override;

  void writeFileHeader();
  void writeSectionHeaderTable();
  void writeSymbolTable();

  // Called after all the csects and symbols have been processed by
  // `executePostLayoutBinding`, this function handles building up the majority
  // of the structures in the object file representation. Namely:
  // *) Calculates physical/virtual addresses, raw-pointer offsets, and section
  //    sizes.
  // *) Assigns symbol table indices.
  // *) Builds up the section header table by adding any non-empty sections to
  //    `Sections`.
  void assignAddressesAndIndices(const llvm::MCAsmLayout &);

  bool
  needsAuxiliaryHeader() const { /* TODO aux header support not implemented. */
    return false;
  }

  // Returns the size of the auxiliary header to be written to the object file.
  size_t auxiliaryHeaderSize() const {
    assert(!needsAuxiliaryHeader() &&
           "Auxiliary header support not implemented.");
    return 0;
  }

public:
  XCOFFObjectWriter(std::unique_ptr<MCXCOFFObjectTargetWriter> MOTW,
                    raw_pwrite_stream &OS);
};

XCOFFObjectWriter::XCOFFObjectWriter(
    std::unique_ptr<MCXCOFFObjectTargetWriter> MOTW, raw_pwrite_stream &OS)
    : W(OS, support::big), TargetObjectWriter(std::move(MOTW)),
      Strings(StringTableBuilder::XCOFF),
      Text(".text", XCOFF::STYP_TEXT, /* IsVirtual */ false),
      BSS(".bss", XCOFF::STYP_BSS, /* IsVirtual */ true) {}

void XCOFFObjectWriter::reset() {
  // Reset any sections we have written to, and empty the section header table.
  for (auto *Sec : Sections)
    Sec->reset();
  Sections.clear();

  // Clear any csects we have stored.
  ProgramCodeCsects.clear();
  BSSCsects.clear();

  // Reset the symbol table and string table.
  SymbolTableEntryCount = 0;
  SymbolTableOffset = 0;
  Strings.clear();

  MCObjectWriter::reset();
}

void XCOFFObjectWriter::executePostLayoutBinding(
    llvm::MCAssembler &Asm, const llvm::MCAsmLayout &Layout) {
  if (TargetObjectWriter->is64Bit())
    report_fatal_error("64-bit XCOFF object files are not supported yet.");

  // Maps the MC Section representation to its corresponding ControlSection
  // wrapper. Needed for finding the ControlSection to insert an MCSymbol into
  // from its containing MCSectionXCOFF.
  DenseMap<const MCSectionXCOFF *, ControlSection *> WrapperMap;

  for (const auto &S : Asm) {
    const auto *MCSec = cast<const MCSectionXCOFF>(&S);
    assert(WrapperMap.find(MCSec) == WrapperMap.end() &&
           "Cannot add a csect twice.");

    switch (MCSec->getMappingClass()) {
    case XCOFF::XMC_PR:
      assert(XCOFF::XTY_SD == MCSec->getCSectType() &&
             "Only an initialized csect can contain program code.");
      // TODO FIXME Handle .text section csects.
      break;
    case XCOFF::XMC_RW:
      if (XCOFF::XTY_CM == MCSec->getCSectType()) {
        BSSCsects.emplace_back(MCSec);
        WrapperMap[MCSec] = &BSSCsects.back();
        break;
      }
      report_fatal_error("Unhandled mapping of read-write csect to section.");
    case XCOFF::XMC_TC0:
      // TODO FIXME Handle emiting the TOC base.
      break;
    case XCOFF::XMC_BS:
      assert(XCOFF::XTY_CM == MCSec->getCSectType() &&
             "Mapping invalid csect. CSECT with bss storage class must be "
             "common type.");
      BSSCsects.emplace_back(MCSec);
      WrapperMap[MCSec] = &BSSCsects.back();
      break;
    default:
      report_fatal_error("Unhandled mapping of csect to section.");
    }
  }

  for (const MCSymbol &S : Asm.symbols()) {
    // Nothing to do for temporary symbols.
    if (S.isTemporary())
      continue;
    const MCSymbolXCOFF *XSym = cast<MCSymbolXCOFF>(&S);

    // Map the symbol into its containing csect.
    const MCSectionXCOFF *ContainingCsect = XSym->getContainingCsect();
    assert(WrapperMap.find(ContainingCsect) != WrapperMap.end() &&
           "Expected containing csect to exist in map");

    // Lookup the containing csect and add the symbol to it.
    WrapperMap[ContainingCsect]->Syms.emplace_back(XSym);

    // If the name does not fit in the storage provided in the symbol table
    // entry, add it to the string table.
    const Symbol &WrapperSym = WrapperMap[ContainingCsect]->Syms.back();
    if (WrapperSym.nameInStringTable()) {
      Strings.add(WrapperSym.getName());
    }
  }

  Strings.finalize();
  assignAddressesAndIndices(Layout);
}

void XCOFFObjectWriter::recordRelocation(MCAssembler &, const MCAsmLayout &,
                                         const MCFragment *, const MCFixup &,
                                         MCValue, uint64_t &) {
  report_fatal_error("XCOFF relocations not supported.");
}

uint64_t XCOFFObjectWriter::writeObject(MCAssembler &Asm, const MCAsmLayout &) {
  // We always emit a timestamp of 0 for reproducibility, so ensure incremental
  // linking is not enabled, in case, like with Windows COFF, such a timestamp
  // is incompatible with incremental linking of XCOFF.
  if (Asm.isIncrementalLinkerCompatible())
    report_fatal_error("Incremental linking not supported for XCOFF.");

  if (TargetObjectWriter->is64Bit())
    report_fatal_error("64-bit XCOFF object files are not supported yet.");

  uint64_t StartOffset = W.OS.tell();

  writeFileHeader();
  writeSectionHeaderTable();
  // TODO writeSections();
  // TODO writeRelocations();

  // TODO FIXME Finalize symbols.
  writeSymbolTable();
  // Write the string table.
  Strings.write(W.OS);

  return W.OS.tell() - StartOffset;
}

void XCOFFObjectWriter::writeFileHeader() {
  // Magic.
  W.write<uint16_t>(0x01df);
  // Number of sections.
  W.write<uint16_t>(Sections.size());
  // Timestamp field. For reproducible output we write a 0, which represents no
  // timestamp.
  W.write<int32_t>(0);
  // Byte Offset to the start of the symbol table.
  W.write<uint32_t>(SymbolTableOffset);
  // Number of entries in the symbol table.
  W.write<int32_t>(SymbolTableEntryCount);
  // Size of the optional header.
  W.write<uint16_t>(0);
  // Flags.
  W.write<uint16_t>(0);
}

void XCOFFObjectWriter::writeSectionHeaderTable() {
  for (const auto *Sec : Sections) {
    // Write Name.
    ArrayRef<char> NameRef(Sec->Name, XCOFF::NameSize);
    W.write(NameRef);

    // Write the Physical Address and Virtual Address. In an object file these
    // are the same.
    W.write<uint32_t>(Sec->Address);
    W.write<uint32_t>(Sec->Address);

    W.write<uint32_t>(Sec->Size);
    W.write<uint32_t>(Sec->FileOffsetToData);

    // Relocation pointer and Lineno pointer. Not supported yet.
    W.write<uint32_t>(0);
    W.write<uint32_t>(0);

    // Relocation and line-number counts. Not supported yet.
    W.write<uint16_t>(0);
    W.write<uint16_t>(0);

    W.write<int32_t>(Sec->Flags);
  }
}

void XCOFFObjectWriter::writeSymbolTable() {
  assert(ProgramCodeCsects.size() == 0 && ".text csects not handled yet.");

  // The BSS Section is special in that the csects must contain a single symbol,
  // and the contained symbol cannot be represented in the symbol table as a
  // label definition.
  for (auto &Sec : BSSCsects) {
    assert(Sec.Syms.size() == 1 &&
           "Uninitialized csect cannot contain more then 1 symbol.");
    Symbol &Sym = Sec.Syms.back();

    // Write the symbol's name.
    if (Sym.nameInStringTable()) {
      W.write<int32_t>(0);
      W.write<uint32_t>(Strings.getOffset(Sym.getName()));
    } else {
      char Name[XCOFF::NameSize];
      std::strncpy(Name, Sym.getName().data(), XCOFF::NameSize);
      ArrayRef<char> NameRef(Name, XCOFF::NameSize);
      W.write(NameRef);
    }

    W.write<uint32_t>(Sec.Address);
    W.write<int16_t>(BSS.Index);
    // Basic/Derived type. See the description of the n_type field for symbol
    // table entries for a detailed description. Since we don't yet support
    // visibility, and all other bits are either optionally set or reserved,
    // this is always zero.
    // TODO FIXME How to assert a symbols visibility is default?
    W.write<uint16_t>(0);

    W.write<uint8_t>(Sym.getStorageClass());

    // Always 1 aux entry for now.
    W.write<uint8_t>(1);

    W.write<uint32_t>(Sec.Size);

    // Parameter typecheck hash. Not supported.
    W.write<uint32_t>(0);
    // Typecheck section number. Not supported.
    W.write<uint16_t>(0);
    // Symbol type.
    W.write<uint8_t>(getEncodedType(Sec.MCCsect));
    // Storage mapping class.
    W.write<uint8_t>(Sec.MCCsect->getMappingClass());
    // Reserved (x_stab).
    W.write<uint32_t>(0);
    // Reserved (x_snstab).
    W.write<uint16_t>(0);
  }
}

void XCOFFObjectWriter::assignAddressesAndIndices(
    const llvm::MCAsmLayout &Layout) {
  // The address corrresponds to the address of sections and symbols in the
  // object file. We place the shared address 0 immediately after the
  // section header table.
  uint32_t Address = 0;
  // Section indices are 1-based in XCOFF.
  uint16_t SectionIndex = 1;
  // The first symbol table entry is for the file name. We are not emitting it
  // yet, so start at index 0.
  uint32_t SymbolTableIndex = 0;

  // Text section comes first. TODO
  // Data section Second. TODO

  // BSS Section third.
  if (!BSSCsects.empty()) {
    Sections.push_back(&BSS);
    BSS.Index = SectionIndex++;
    assert(alignTo(Address, DefaultSectionAlign) == Address &&
           "Improperly aligned address for section.");
    uint32_t StartAddress = Address;
    for (auto &Csect : BSSCsects) {
      const MCSectionXCOFF *MCSec = Csect.MCCsect;
      Address = alignTo(Address, MCSec->getAlignment());
      Csect.Address = Address;
      Address += Layout.getSectionAddressSize(MCSec);
      Csect.SymbolTableIndex = SymbolTableIndex;
      // 1 main and 1 auxiliary symbol table entry for the csect.
      SymbolTableIndex += 2;
      Csect.Size = Layout.getSectionAddressSize(MCSec);

      assert(Csect.Syms.size() == 1 &&
             "csect in the BSS can only contain a single symbol.");
      Csect.Syms[0].SymbolTableIndex = Csect.SymbolTableIndex;
    }
    // Pad out Address to the default alignment. This is to match how the system
    // assembler handles the .bss section. Its size is always a multiple of 4.
    Address = alignTo(Address, DefaultSectionAlign);
    BSS.Size = Address - StartAddress;
  }

  SymbolTableEntryCount = SymbolTableIndex;

  // Calculate the RawPointer value for each section.
  uint64_t RawPointer = sizeof(XCOFF::FileHeader32) + auxiliaryHeaderSize() +
                        Sections.size() * sizeof(XCOFF::SectionHeader32);
  for (auto *Sec : Sections) {
    if (!Sec->IsVirtual) {
      Sec->FileOffsetToData = RawPointer;
      RawPointer += Sec->Size;
    }
  }

  // TODO Add in Relocation storage to the RawPointer Calculation.
  // TODO What to align the SymbolTable to?
  // TODO Error check that the number of symbol table entries fits in 32-bits
  // signed ...
  if (SymbolTableEntryCount)
    SymbolTableOffset = RawPointer;
}

// Takes the log base 2 of the alignment and shifts the result into the 5 most
// significant bits of a byte, then or's in the csect type into the least
// significant 3 bits.
uint8_t getEncodedType(const MCSectionXCOFF *Sec) {
  unsigned Align = Sec->getAlignment();
  assert(isPowerOf2_32(Align) && "Alignment must be a power of 2.");
  unsigned Log2Align = Log2_32(Align);
  // Result is a number in the range [0, 31] which fits in the 5 least
  // significant bits. Shift this value into the 5 most significant bits, and
  // bitwise-or in the csect type.
  uint8_t EncodedAlign = Log2Align << 3;
  return EncodedAlign | Sec->getCSectType();
}

} // end anonymous namespace

std::unique_ptr<MCObjectWriter>
llvm::createXCOFFObjectWriter(std::unique_ptr<MCXCOFFObjectTargetWriter> MOTW,
                              raw_pwrite_stream &OS) {
  return std::make_unique<XCOFFObjectWriter>(std::move(MOTW), OS);
}
