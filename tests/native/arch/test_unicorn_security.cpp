#include <doctest/doctest.h>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace
{
constexpr std::uint64_t code_base  = 0x0800'0000;
constexpr std::uint64_t code_size  = 0x1'0000;
constexpr std::uint64_t sram_base  = 0x2000'0000;
constexpr std::uint64_t sram_size  = 0x1'0000;
constexpr std::uint64_t sram_alias = 0x3000'0000;
constexpr std::uint32_t secure_sp  = 0x2000'8000;
constexpr std::uint32_t fnc_return = 0xFEFF'FFFF;
constexpr std::size_t   step_bound = 16;

constexpr std::uint32_t excp_prefetch_abort = 3;
constexpr std::uint32_t excp_exception_exit = 8;
constexpr std::uint32_t excp_nocp           = 17;

constexpr std::uint32_t control_fpca = 1u << 2;
constexpr std::uint32_t control_sfpa = 1u << 3;

constexpr std::uint32_t tt_srvalid = 1u << 17;
constexpr std::uint32_t tt_r       = 1u << 18;
constexpr std::uint32_t tt_rw      = 1u << 19;
constexpr std::uint32_t tt_s       = 1u << 22;

constexpr std::uint32_t sau_enable = 1u << 0;
constexpr std::uint32_t sau_allns  = 1u << 1;

constexpr std::uint32_t cpacr_cp10_cp11 = 0x00f0'0000;
constexpr std::uint32_t fpccr_s_eager   = 0x8000'0004;

struct engine_closer
{
  void operator()(uc_engine* engine) const noexcept
  {
    uc_close(engine);
  }
};

struct trap
{
  std::uint32_t intno;
  std::uint32_t pc;
};

class engine
{
public:
  engine()
  {
    uc_engine* raw = nullptr;
    REQUIRE(
        uc_open(UC_ARCH_ARM, static_cast<uc_mode>(UC_MODE_THUMB | UC_MODE_MCLASS), &raw)
        == UC_ERR_OK);
    handle_.reset(raw);
    REQUIRE(uc_ctl_set_cpu_model(raw, UC_CPU_ARM_CORTEX_M33) == UC_ERR_OK);
    REQUIRE(uc_mem_map(raw, code_base, code_size, UC_PROT_ALL) == UC_ERR_OK);
    REQUIRE(uc_mem_map(raw, sram_base, sram_size, UC_PROT_ALL) == UC_ERR_OK);
    REQUIRE(uc_mem_map_alias(raw, sram_alias, sram_size, UC_PROT_ALL, sram_base) == UC_ERR_OK);
    uc_hook hook{};
    REQUIRE(
        uc_hook_add(raw, &hook, UC_HOOK_INTR, reinterpret_cast<void*>(&on_interrupt), this, 1, 0)
        == UC_ERR_OK);
    set(UC_ARM_REG_SP, secure_sp);
  }

  engine(engine&&) = delete;

  [[nodiscard]] auto raw() const -> uc_engine*
  {
    return handle_.get();
  }

  void load(std::uint64_t at, std::initializer_list<std::uint16_t> halves) const
  {
    load(at, std::span{halves.begin(), halves.size()});
  }

  void load(std::uint64_t at, std::span<std::uint16_t const> halves) const
  {
    std::vector<std::byte> bytes;
    for(auto const half : halves)
    {
      bytes.push_back(static_cast<std::byte>(half & 0xffu));
      bytes.push_back(static_cast<std::byte>(half >> 8));
    }
    REQUIRE(uc_mem_write(raw(), at, bytes.data(), bytes.size()) == UC_ERR_OK);
  }

  void set(int reg, std::uint32_t value) const
  {
    REQUIRE(uc_reg_write(raw(), reg, &value) == UC_ERR_OK);
  }

  [[nodiscard]] auto get(int reg) const -> std::uint32_t
  {
    std::uint32_t value = 0;
    REQUIRE(uc_reg_read(raw(), reg, &value) == UC_ERR_OK);
    return value;
  }

  void put_word(std::uint64_t at, std::uint32_t value) const
  {
    REQUIRE(uc_mem_write(raw(), at, &value, sizeof value) == UC_ERR_OK);
  }

  [[nodiscard]] auto word(std::uint64_t at) const -> std::uint32_t
  {
    std::uint32_t value = 0;
    REQUIRE(uc_mem_read(raw(), at, &value, sizeof value) == UC_ERR_OK);
    return value;
  }

  auto run(std::uint64_t from, std::uint64_t until, std::size_t count = step_bound)
      -> std::optional<trap>
  {
    trap_.reset();
    REQUIRE(uc_emu_start(raw(), from | 1u, until, 0, count) == UC_ERR_OK);
    return trap_;
  }

private:
  static void on_interrupt(uc_engine* uc, std::uint32_t intno, void* user)
  {
    auto&         self = *static_cast<engine*>(user);
    std::uint32_t pc   = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    if(not self.trap_)
    {
      self.trap_ = trap{.intno = intno, .pc = pc};
    }
    uc_emu_stop(uc);
  }

  std::unique_ptr<uc_engine, engine_closer> handle_;
  std::optional<trap>                       trap_;
};

void secure_first_page(void*, std::uint32_t address, int*, bool*, bool* ns, bool* nsc)
{
  if(address >= code_base and address < code_base + 0x400)
  {
    *ns  = false;
    *nsc = false;
  }
}

auto fnc_return_trap(std::span<std::uint16_t const> code, std::uint32_t r0) -> std::optional<trap>
{
  engine e;
  e.load(code_base, code);
  e.put_word(sram_base, fnc_return);
  e.put_word(sram_base + 4, fnc_return);
  e.put_word(secure_sp, fnc_return);
  e.put_word(secure_sp + 4, fnc_return);
  e.set(UC_ARM_REG_LR, fnc_return);
  e.set(UC_ARM_REG_R0, r0);
  return e.run(code_base, code_base + 2 * code.size());
}
}

TEST_CASE("the engine page size is 1 KB")
{
  engine        e;
  std::uint32_t size = 0;
  REQUIRE(uc_ctl_get_page_size(e.raw(), &size) == UC_ERR_OK);
  MESSAGE("page size " << size);
  CHECK(size == 1024);
}

TEST_CASE("Secure stores and loads through 0x2 and its 0x3 alias succeed with the lookup off")
{
  engine e;
  e.load(code_base, {0x6001, 0x6802});
  e.set(UC_ARM_REG_R0, static_cast<std::uint32_t>(sram_alias + 0x100));
  e.set(UC_ARM_REG_R1, 0x1234'5678);
  CHECK_FALSE(e.run(code_base, code_base + 4).has_value());
  CHECK(e.get(UC_ARM_REG_R2) == 0x1234'5678);
  CHECK(e.word(sram_base + 0x100) == 0x1234'5678);
}

TEST_CASE("BXNS to an even address switches to Non-secure, then the fetch faults, B3.15 RPQHT")
{
  engine e;
  e.load(code_base, {0x470c});
  e.set(UC_ARM_REG_R1, static_cast<std::uint32_t>(code_base + 0x100));
  auto const trapped = e.run(code_base, code_base + 2);
  REQUIRE(trapped.has_value());
  CHECK(trapped->intno == excp_prefetch_abort);
  CHECK(trapped->pc == code_base + 0x100);
  CHECK(e.get(UC_ARM_REG_SP) == 0);
}

TEST_CASE("BXNS to an even address clears CONTROL_S.SFPA, RFVTL")
{
  engine e;
  e.load(code_base, {0xee00, 0x2a10, 0x470c});
  e.set(UC_ARM_REG_R1, static_cast<std::uint32_t>(code_base + 0x100));
  e.set(UC_ARM_REG_R2, 0x3f80'0000);
  auto const trapped = e.run(code_base, code_base + 6);
  REQUIRE(trapped.has_value());
  CHECK(trapped->intno == excp_prefetch_abort);
  CHECK(e.get(UC_ARM_REG_SECURE) == 0);
  CHECK(e.get(UC_ARM_REG_CONTROL_S) == control_fpca);
}

TEST_CASE("BXNS fetches its Non-secure target with the lookup on, SAU ALLNS, B3.15 RPQHT")
{
  engine e;
  e.load(code_base, {0x470c});
  e.load(code_base + 0x800, {0x205a});
  e.set(UC_ARM_REG_R1, static_cast<std::uint32_t>(code_base + 0x800));
  REQUIRE(uc_ctl_arm_idau(e.raw(), &secure_first_page, nullptr) == UC_ERR_OK);
  REQUIRE(uc_ctl_arm_security(e.raw(), 1) == UC_ERR_OK);
  e.set(UC_ARM_REG_SAU_CTRL, sau_allns);
  CHECK_FALSE(e.run(code_base, code_base + 0x802).has_value());
  CHECK(e.get(UC_ARM_REG_R0) == 0x5a);
  CHECK(e.get(UC_ARM_REG_SECURE) == 0);
}

TEST_CASE("BLXNS from a Secure handler pushes ReturnAddress and partial RETPSR, B3.16 RGVBB")
{
  engine e;
  e.load(code_base + 0x10, {0x478c});
  e.set(UC_ARM_REG_IPSR, 3);
  e.set(UC_ARM_REG_R1, static_cast<std::uint32_t>(code_base + 0x200));
  auto const trapped = e.run(code_base + 0x10, code_base + 0x12);
  REQUIRE(trapped.has_value());
  CHECK(trapped->intno == excp_prefetch_abort);
  CHECK(trapped->pc == code_base + 0x200);
  CHECK(e.word(secure_sp - 8) == code_base + 0x13);
  CHECK(e.word(secure_sp - 4) == 3);
  CHECK(e.get(UC_ARM_REG_LR) == fnc_return);
  CHECK(e.get(UC_ARM_REG_IPSR) == 1);
}

TEST_CASE("BLXNS stacks CONTROL_S.SFPA in RETPSR bit 20, B3.16 RGVBB")
{
  engine e;
  e.load(code_base + 0x10, {0xee00, 0x2a10, 0x478c});
  e.set(UC_ARM_REG_IPSR, 3);
  e.set(UC_ARM_REG_R1, static_cast<std::uint32_t>(code_base + 0x200));
  auto const trapped = e.run(code_base + 0x10, code_base + 0x16);
  REQUIRE(trapped.has_value());
  CHECK(trapped->intno == excp_prefetch_abort);
  CHECK(e.word(secure_sp - 8) == code_base + 0x17);
  CHECK(e.word(secure_sp - 4) == (3u | 1u << 20));
}

TEST_CASE("FNC_RETURN from Secure state raises EXCP_EXCEPTION_EXIT in every branch form, B3.17")
{
  struct form
  {
    std::string_view           name;
    std::vector<std::uint16_t> code;
    std::uint32_t              r0;
  };
  auto const forms = std::array{
      form{"BX LR",             {0x4770},         0                                    },
      form{"BXNS LR",           {0x4774},         0                                    },
      form{"POP {pc}",          {0xbd00},         0                                    },
      form{"POP.W {r4, pc}",    {0xe8bd, 0x8010}, 0                                    },
      form{"LDM r0!, {r1, pc}", {0xe8b0, 0x8002}, static_cast<std::uint32_t>(sram_base)},
      form{"LDR.W pc, [r0]",    {0xf8d0, 0xf000}, static_cast<std::uint32_t>(sram_base)},
  };
  for(auto const& one : forms)
  {
    CAPTURE(one.name);
    auto const trapped = fnc_return_trap(one.code, one.r0);
    REQUIRE(trapped.has_value());
    CHECK(trapped->intno == excp_exception_exit);
    CHECK(trapped->pc == (fnc_return & ~1u));
  }
}

TEST_CASE("SG executed in Secure state is a NOP, C2.4.180")
{
  engine e;
  e.load(code_base, {0xe97f, 0xe97f, 0xbf00});
  CHECK_FALSE(e.run(code_base, code_base + 6).has_value());
  CHECK(e.get(UC_ARM_REG_PC) == code_base + 6);
}

TEST_CASE("TT, TTT and TTA with the security lookup off, or on with SAU_CTRL 0, C2.4.254")
{
  struct form
  {
    std::string_view name;
    std::uint16_t    second;
  };
  auto const forms = std::array{
      form{"TT",  0xf000},
      form{"TTT", 0xf040},
      form{"TTA", 0xf080},
  };
  auto const targets = std::array<std::uint32_t, 3>{0x0800'0000, 0x2000'0000, 0x3000'0000};
  for(auto const lookup : {0, 1})
  {
    for(auto const& one : forms)
    {
      for(auto const target : targets)
      {
        CAPTURE(lookup);
        CAPTURE(one.name);
        CAPTURE(target);
        engine e;
        REQUIRE(uc_ctl_arm_security(e.raw(), lookup) == UC_ERR_OK);
        e.load(code_base, {0xe841, one.second});
        e.set(UC_ARM_REG_R1, target);
        e.set(UC_ARM_REG_R0, 0xdead'beef);
        CHECK_FALSE(e.run(code_base, code_base + 4).has_value());
        CHECK(e.get(UC_ARM_REG_R0) == (tt_r | tt_rw | tt_s));
      }
    }
  }
}

TEST_CASE("TT from Secure reports SRVALID and the SAU region, C2.4.254 and B10.3 RKRSC")
{
  engine e;
  REQUIRE(uc_ctl_arm_security(e.raw(), 1) == UC_ERR_OK);
  e.set(UC_ARM_REG_SAU_RNR, 3);
  e.set(UC_ARM_REG_SAU_RBAR, static_cast<std::uint32_t>(sram_base));
  e.set(UC_ARM_REG_SAU_RLAR, static_cast<std::uint32_t>(sram_base + 0x3e0) | sau_enable);
  e.set(UC_ARM_REG_SAU_RNR, 5);
  e.set(UC_ARM_REG_SAU_CTRL, sau_enable);
  e.load(code_base, {0xe841, 0xf000});
  e.set(UC_ARM_REG_R1, static_cast<std::uint32_t>(sram_base + 0x100));
  CHECK_FALSE(e.run(code_base, code_base + 4).has_value());
  auto const result = e.get(UC_ARM_REG_R0);
  CHECK((result & tt_srvalid) != 0);
  CHECK(((result >> 8) & 0xffu) == 3);
  CHECK((result & tt_s) == 0);
}

TEST_CASE("an FP instruction in Secure state sets CONTROL.FPCA and CONTROL_S.SFPA, B3.4")
{
  engine e;
  e.load(code_base, {0xee00, 0x2a10});
  e.set(UC_ARM_REG_R2, 0x3f80'0000);
  REQUIRE(e.get(UC_ARM_REG_CONTROL) == 0);
  CHECK_FALSE(e.run(code_base, code_base + 4).has_value());
  CHECK(e.get(UC_ARM_REG_CONTROL) == (control_fpca | control_sfpa));
}

namespace
{
struct fp_context_form
{
  std::string_view name;
  std::uint16_t    first;
};

constexpr auto fp_context_forms = std::array{
    fp_context_form{"VLSTM", 0xec20},
    fp_context_form{"VLLDM", 0xec30},
};
constexpr std::uint64_t fp_frame  = sram_base + 0x2000;
constexpr std::uint32_t untouched = 0xa5a5'a5a5;
}

TEST_CASE("VLSTM and VLLDM with CONTROL_S.SFPA clear do nothing, C2.4.368 and C2.4.367")
{
  for(auto const& one : fp_context_forms)
  {
    CAPTURE(one.name);
    engine e;
    e.load(code_base, {one.first, 0x0a00});
    for(std::uint64_t at = fp_frame; at < fp_frame + 0x88; at += 4)
    {
      e.put_word(at, untouched);
    }
    e.set(UC_ARM_REG_S0, 0x3f80'0000);
    e.set(UC_ARM_REG_R0, static_cast<std::uint32_t>(fp_frame));
    e.set(UC_ARM_REG_CONTROL, control_fpca);
    CHECK_FALSE(e.run(code_base, code_base + 4).has_value());
    CHECK(e.word(fp_frame) == untouched);
    CHECK(e.word(fp_frame + 0x40) == untouched);
    CHECK(e.get(UC_ARM_REG_S0) == 0x3f80'0000);
    CHECK(e.get(UC_ARM_REG_CONTROL) == control_fpca);
  }
}

TEST_CASE("VLSTM with CPACR_S CP10 set and FPCCR_S.LSPEN 0 saves the FP context, C2.4.368")
{
  engine e;
  e.load(code_base, {0xee00, 0x2a10, 0xeee1, 0x3a10, 0xec20, 0x0a00});
  for(std::uint64_t at = fp_frame; at < fp_frame + 0x88; at += 4)
  {
    e.put_word(at, untouched);
  }
  e.set(UC_ARM_REG_R0, static_cast<std::uint32_t>(fp_frame));
  e.set(UC_ARM_REG_R2, 0x3f80'0000);
  e.set(UC_ARM_REG_R3, 0x00c0'0000);
  e.set(UC_ARM_REG_CPACR_S, cpacr_cp10_cp11);
  e.set(UC_ARM_REG_FPCCR_S, fpccr_s_eager);
  CHECK_FALSE(e.run(code_base, code_base + 12).has_value());
  CHECK(e.word(fp_frame) == 0x3f80'0000);
  CHECK(e.word(fp_frame + 0x40) == 0x00c0'0000);
  CHECK(e.word(fp_frame + 0x44) == untouched);
  CHECK((e.get(UC_ARM_REG_CONTROL) & control_fpca) == 0);
  CHECK(e.get(UC_ARM_REG_FPCCR_S) == fpccr_s_eager);
}

TEST_CASE("VLSTM and VLLDM with CONTROL_S.SFPA set raise NOCP, C2.4.368 and C2.4.367")
{
  for(auto const& one : fp_context_forms)
  {
    for(auto const fpca : {control_fpca, 0u})
    {
      CAPTURE(one.name);
      CAPTURE(fpca);
      engine e;
      e.load(code_base, {one.first, 0x0a00});
      e.put_word(fp_frame, untouched);
      e.set(UC_ARM_REG_R0, static_cast<std::uint32_t>(fp_frame));
      e.set(UC_ARM_REG_CPACR_S, 0);
      e.set(UC_ARM_REG_CONTROL, control_sfpa | fpca);
      auto const trapped = e.run(code_base, code_base + 4);
      REQUIRE(trapped.has_value());
      CHECK(trapped->intno == excp_nocp);
      CHECK(trapped->pc == code_base);
      CHECK(e.word(fp_frame) == untouched);
      CHECK(e.get(UC_ARM_REG_CONTROL) == (control_sfpa | fpca));
    }
  }
}

TEST_CASE("Secure load and store loop timing on the TLB fast path" * doctest::skip())
{
  constexpr std::uint32_t  iterations = 10'000'000;
  constexpr int            runs       = 5;
  std::array<double, runs> ns_per_iteration{};
  for(auto& result : ns_per_iteration)
  {
    engine e;
    e.load(code_base, {0x6001, 0x6802, 0x3b01, 0xd1fb});
    e.set(UC_ARM_REG_R0, static_cast<std::uint32_t>(sram_base + 0x1000));
    e.set(UC_ARM_REG_R3, iterations);
    auto const start = std::chrono::steady_clock::now();
    REQUIRE_FALSE(e.run(code_base, code_base + 8, 0).has_value());
    auto const elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(e.get(UC_ARM_REG_R3) == 0);
    result = std::chrono::duration<double, std::nano>(elapsed).count() / iterations;
  }
  for(auto const result : ns_per_iteration)
  {
    std::printf("ns per iteration %.3f\n", result);
  }
  std::ranges::sort(ns_per_iteration);
  std::printf(
      "median %.3f spread %.3f\n", ns_per_iteration[runs / 2],
      ns_per_iteration.back() - ns_per_iteration.front());
}

namespace
{
struct alignment_probe
{
  int           intno{-1};
  std::uint32_t status{};
  std::uint32_t pc{};
  uc_err        status_read{UC_ERR_OK};
  uc_err        pc_read{UC_ERR_OK};
  uc_err        stopped{UC_ERR_OK};
};

void on_alignment_abort(uc_engine* uc, std::uint32_t intno, void* user)
{
  auto& probe       = *static_cast<alignment_probe*>(user);
  probe.intno       = static_cast<int>(intno);
  probe.status_read = uc_reg_read(uc, UC_ARM_REG_FAULT_STATUS, &probe.status);
  probe.pc_read     = uc_reg_read(uc, UC_ARM_REG_PC, &probe.pc);
  probe.stopped     = uc_emu_stop(uc);
}

auto run_unaligned(uc_cpu_arm model, bool mclass, std::initializer_list<std::uint16_t> code)
    -> alignment_probe
{
  uc_engine* raw  = nullptr;
  auto const mode = mclass ? static_cast<uc_mode>(UC_MODE_THUMB | UC_MODE_MCLASS) : UC_MODE_THUMB;
  REQUIRE(uc_open(UC_ARCH_ARM, mode, &raw) == UC_ERR_OK);
  std::unique_ptr<uc_engine, engine_closer> const owned{raw};
  REQUIRE(uc_ctl_set_cpu_model(raw, model) == UC_ERR_OK);
  REQUIRE(uc_mem_map(raw, code_base, code_size, UC_PROT_ALL) == UC_ERR_OK);
  REQUIRE(uc_mem_map(raw, sram_base, sram_size, UC_PROT_ALL) == UC_ERR_OK);
  std::vector<std::uint16_t> const words{code};
  REQUIRE(uc_mem_write(raw, code_base, words.data(), words.size() * 2) == UC_ERR_OK);
  alignment_probe probe;
  uc_hook         hook{};
  REQUIRE(
      uc_hook_add(
          raw, &hook, UC_HOOK_INTR, reinterpret_cast<void*>(&on_alignment_abort), &probe, 1, 0)
      == UC_ERR_OK);
  std::uint32_t const at = sram_base + 0x101;
  REQUIRE(uc_reg_write(raw, UC_ARM_REG_R0, &at) == UC_ERR_OK);
  REQUIRE(
      uc_emu_start(raw, code_base | 1, code_base + 2 * words.size(), 0, step_bound) == UC_ERR_OK);
  CHECK(probe.status_read == UC_ERR_OK);
  CHECK(probe.pc_read == UC_ERR_OK);
  CHECK(probe.stopped == UC_ERR_OK);
  return probe;
}
}

TEST_CASE("the engine raises an alignment data abort, status 0x1, on every core model")
{
  constexpr std::uint32_t excp_data_abort = 4;
  constexpr std::uint16_t ldr             = 0x6801;
  struct row
  {
    std::string_view                     cpu;
    uc_cpu_arm                           model;
    bool                                 mclass;
    std::initializer_list<std::uint16_t> code;
    std::uint32_t                        status;
  };
  for(auto const& one : std::initializer_list<row>{
          {"CM0 ldr",     UC_CPU_ARM_CORTEX_M0,  false, {ldr},            0x1},
          {"CM3 ldrex",   UC_CPU_ARM_CORTEX_M3,  false, {0xe850, 0x1f00}, 0x1},
          {"CM4 ldrex",   UC_CPU_ARM_CORTEX_M4,  false, {0xe850, 0x1f00}, 0x1},
          {"CM7 ldrex",   UC_CPU_ARM_CORTEX_M7,  false, {0xe850, 0x1f00}, 0x1},
          {"CM33 ldrex",  UC_CPU_ARM_CORTEX_M33, true,  {0xe850, 0x1f00}, 0x1},
          {"CM33 ldrexh", UC_CPU_ARM_CORTEX_M33, true,  {0xe8d0, 0x1f5f}, 0x1},
  })
  {
    CAPTURE(one.cpu);
    auto const probe = run_unaligned(one.model, one.mclass, one.code);
    CHECK(probe.intno == static_cast<int>(excp_data_abort));
    CHECK((probe.status & 0x40f) == 0x1);
    CHECK(probe.status == one.status);
    CHECK(probe.pc == code_base);
  }
  for(auto const& [model, mclass] : {
          std::pair{UC_CPU_ARM_CORTEX_M4,  false},
          std::pair{UC_CPU_ARM_CORTEX_M33, true }
  })
  {
    CAPTURE(model);
    CHECK(run_unaligned(model, mclass, {ldr}).intno == -1);
  }
}
