#pragma once

#include <elf.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::testing
{
/** @brief Builds a minimal little-endian ARM ELF32 file in memory for image loader edge cases. */
class synthetic_elf
{
public:
  synthetic_elf()
  {
    std::memset(&header_, 0, sizeof header_);
    header_.e_ident[EI_MAG0]    = ELFMAG0;
    header_.e_ident[EI_MAG1]    = ELFMAG1;
    header_.e_ident[EI_MAG2]    = ELFMAG2;
    header_.e_ident[EI_MAG3]    = ELFMAG3;
    header_.e_ident[EI_CLASS]   = ELFCLASS32;
    header_.e_ident[EI_DATA]    = ELFDATA2LSB;
    header_.e_ident[EI_VERSION] = EV_CURRENT;
    header_.e_type              = ET_EXEC;
    header_.e_machine           = EM_ARM;
    header_.e_version           = EV_CURRENT;
    header_.e_ehsize            = sizeof(Elf32_Ehdr);
    header_.e_phentsize         = sizeof(Elf32_Phdr);
    header_.e_shentsize         = sizeof(Elf32_Shdr);
  }

  auto header() -> Elf32_Ehdr&
  {
    return header_;
  }

  auto add_segment(
      std::uint32_t                 paddr,
      std::uint32_t                 vaddr,
      std::vector<std::byte> const& data,
      std::uint32_t                 memsz,
      std::uint32_t                 flags = PF_R | PF_X) -> synthetic_elf&
  {
    Elf32_Phdr ph{};
    ph.p_type   = PT_LOAD;
    ph.p_paddr  = paddr;
    ph.p_vaddr  = vaddr;
    ph.p_filesz = static_cast<Elf32_Word>(data.size());
    ph.p_memsz  = memsz;
    ph.p_flags  = flags;
    ph.p_align  = 1;
    phdrs_.push_back(ph);
    payloads_.push_back(data);
    return *this;
  }

  /**
   * @brief Adds a section with its own file content.
   * @return This builder's own index for the new section, 1-based (index 0 is the reserved
   *         NULL section), usable as an @c sh_link target.
   */
  auto add_section(
      std::string            name,
      Elf32_Word             type,
      Elf32_Word             flags,
      std::vector<std::byte> data,
      Elf32_Word             link = 0) -> std::size_t
  {
    section_names_.push_back(std::move(name));
    section_payloads_.push_back(std::move(data));
    Elf32_Shdr sh{};
    sh.sh_type      = type;
    sh.sh_flags     = flags;
    sh.sh_link      = link;
    sh.sh_addralign = 1;
    shdrs_.push_back(sh);
    return shdrs_.size();
  }

  /** @brief Byte offset of shdr @p index (0 = NULL section) once built, for field patching. */
  auto shdr_file_offset(std::size_t index) const -> std::size_t
  {
    return layout().shoff + index * sizeof(Elf32_Shdr);
  }

  auto build() const -> std::vector<std::byte>
  {
    auto const l = layout();

    Elf32_Ehdr eh = header_;
    eh.e_phnum    = static_cast<Elf32_Half>(phdrs_.size());
    eh.e_phoff    = static_cast<Elf32_Word>(l.phdr_off);

    std::vector<Elf32_Phdr> phdrs = phdrs_;
    for(std::size_t i = 0; i < phdrs.size(); ++i)
    {
      phdrs[i].p_offset = static_cast<Elf32_Word>(l.payload_offsets[i]);
    }

    std::vector<std::byte> out(l.total_size);
    std::memcpy(out.data(), &eh, sizeof eh);
    for(std::size_t i = 0; i < phdrs.size(); ++i)
    {
      std::memcpy(out.data() + l.phdr_off + i * sizeof(Elf32_Phdr), &phdrs[i], sizeof(Elf32_Phdr));
      std::memcpy(out.data() + l.payload_offsets[i], payloads_[i].data(), payloads_[i].size());
    }

    if(!shdrs_.empty())
    {
      for(std::size_t i = 0; i < shdrs_.size(); ++i)
      {
        std::memcpy(
            out.data() + l.section_payload_offsets[i], section_payloads_[i].data(),
            section_payloads_[i].size());
      }
      std::memcpy(out.data() + l.shstrtab_offset, l.shstrtab.data(), l.shstrtab.size());

      auto write_shdr = [&](std::size_t index, Elf32_Shdr sh) {
        std::memcpy(out.data() + l.shoff + index * sizeof(Elf32_Shdr), &sh, sizeof sh);
      };
      write_shdr(0, Elf32_Shdr{});
      for(std::size_t i = 0; i < shdrs_.size(); ++i)
      {
        Elf32_Shdr sh = shdrs_[i];
        sh.sh_name    = static_cast<Elf32_Word>(l.name_offsets[i]);
        sh.sh_offset  = static_cast<Elf32_Word>(l.section_payload_offsets[i]);
        sh.sh_size    = static_cast<Elf32_Word>(section_payloads_[i].size());
        write_shdr(1 + i, sh);
      }
      Elf32_Shdr shstrtab_sh{};
      shstrtab_sh.sh_type   = SHT_STRTAB;
      shstrtab_sh.sh_name   = static_cast<Elf32_Word>(l.shstrtab_name_offset);
      shstrtab_sh.sh_offset = static_cast<Elf32_Word>(l.shstrtab_offset);
      shstrtab_sh.sh_size   = static_cast<Elf32_Word>(l.shstrtab.size());
      write_shdr(1 + shdrs_.size(), shstrtab_sh);

      auto eh2        = eh;
      eh2.e_shoff     = static_cast<Elf32_Word>(l.shoff);
      eh2.e_shnum     = static_cast<Elf32_Half>(2 + shdrs_.size());
      eh2.e_shentsize = sizeof(Elf32_Shdr);
      eh2.e_shstrndx  = static_cast<Elf32_Half>(1 + shdrs_.size());
      std::memcpy(out.data(), &eh2, sizeof eh2);
    }
    return out;
  }

  static auto write(std::vector<std::byte> const& bytes, std::string const& name)
      -> std::filesystem::path
  {
    char const* const           scratch = std::getenv("POPCORN_IMAGE_SCRATCH");
    std::filesystem::path const dir =
        scratch != nullptr ? scratch : std::filesystem::temp_directory_path();
    std::filesystem::create_directories(dir);
    auto const    path = dir / name;
    std::ofstream out{path, std::ios::binary};
    out.write(
        reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
  }

private:
  struct layout_t
  {
    std::size_t              phdr_off{};
    std::vector<std::size_t> payload_offsets;
    std::vector<std::size_t> section_payload_offsets;
    std::vector<std::size_t> name_offsets;
    std::vector<std::byte>   shstrtab;
    std::size_t              shstrtab_name_offset{};
    std::size_t              shstrtab_offset{};
    std::size_t              shoff{};
    std::size_t              total_size{};
  };

  auto layout() const -> layout_t
  {
    layout_t l;
    l.phdr_off         = sizeof(Elf32_Ehdr);
    std::size_t cursor = l.phdr_off + phdrs_.size() * sizeof(Elf32_Phdr);

    l.payload_offsets.resize(payloads_.size());
    for(std::size_t i = 0; i < payloads_.size(); ++i)
    {
      l.payload_offsets[i] = cursor;
      cursor += payloads_[i].size();
    }

    if(!shdrs_.empty())
    {
      l.section_payload_offsets.resize(section_payloads_.size());
      for(std::size_t i = 0; i < section_payloads_.size(); ++i)
      {
        l.section_payload_offsets[i] = cursor;
        cursor += section_payloads_[i].size();
      }

      l.shstrtab.push_back(std::byte{0});
      l.shstrtab_name_offset = l.shstrtab.size();
      for(char c : std::string_view{".shstrtab"})
      {
        l.shstrtab.push_back(static_cast<std::byte>(c));
      }
      l.shstrtab.push_back(std::byte{0});
      l.name_offsets.resize(section_names_.size());
      for(std::size_t i = 0; i < section_names_.size(); ++i)
      {
        l.name_offsets[i] = l.shstrtab.size();
        for(char c : section_names_[i])
        {
          l.shstrtab.push_back(static_cast<std::byte>(c));
        }
        l.shstrtab.push_back(std::byte{0});
      }

      l.shstrtab_offset = cursor;
      cursor += l.shstrtab.size();

      cursor += (4 - cursor % 4) % 4;
      l.shoff = cursor;
      cursor += (2 + shdrs_.size()) * sizeof(Elf32_Shdr);
    }

    l.total_size = cursor;
    return l;
  }

  Elf32_Ehdr                          header_{};
  std::vector<Elf32_Phdr>             phdrs_;
  std::vector<std::vector<std::byte>> payloads_;
  std::vector<std::string>            section_names_;
  std::vector<std::vector<std::byte>> section_payloads_;
  std::vector<Elf32_Shdr>             shdrs_;
};
}
