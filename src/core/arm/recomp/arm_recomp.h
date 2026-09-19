// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include "core/arm/arm_interface.h"

namespace Kernel {
class KProcess;
}

namespace Core {

class System;
class DynarmicExclusiveMonitor;

/**
 * Signature of a recompiled block produced by suyu::recomp::EmitProject.
 *
 * The generated code is plain C operating on its own GuestContext, but the
 * only state it actually needs from us is the register file, PC and NZCV, so
 * the bridge below re-declares a layout-compatible view of that prefix rather
 * than pulling the generated header into the emulator build.
 */
struct RecompGuestRegs {
    u64 x[32];
    u64 pc;
    u8 n, z, c, v;
};

using RecompBlockFn = void (*)(void*);
using RecompLookupFn = RecompBlockFn (*)(u64 pc);

/**
 * Registers the lookup function of a loaded recompiled image.
 *
 * Set before starting a process to have it run on ArmRecomp instead of the
 * JIT; pass nullptr to go back to the normal backend. Kept as a global rather
 * than threaded through the loader because the choice is per-image and has to
 * be visible at KProcess::InitializeInterfaces time, which the loader doesn't
 * own.
 */
void SetRecompLookup(RecompLookupFn lookup);

/// Called once per loaded module when a process starts, so each recompiled
/// image can be told where its module actually landed. Addresses baked in by
/// the static pass are module-relative - the loader picks the real base at run
/// time - so without this every pointer the guest computes is short by that
/// base and lands near null.
/// `index` is the module's position in load order, lowest base first. Names
/// cannot be relied on to identify a module: a game's main NSO is named after
/// the game ("cross2_Release.nss"), and its sdk and subsdk modules carry names
/// like "nnSdk" and "multimedia", none of which match the file names the
/// exporter used. Load order is the same on every title - rtld, main, the
/// subsdks, then sdk - so the index is what actually lines up.
using RecompBaseFn = void (*)(size_t index, const char* module, u64 base);
void SetRecompBaseSetter(RecompBaseFn setter);

/// Returns the registered lookup, or nullptr when no recompiled image is
/// loaded and the JIT should be used.
RecompLookupFn GetRecompLookup();

/// schema_version 1 AOT vs Dynarmic execution snapshot.
///
/// Totals are run-lifetime: they survive ArmRecomp attaching a new application
/// process (the stack-harness restart path). Times are host steady_clock
/// nanoseconds around real AOT block entry and Dynarmic RunThread/StepThread,
/// not simulated stubs.
///
/// JSON (FormatRecompExecutionJson): kind=recomp_execution, clock=steady_clock,
/// backends.{aot,dynarmic}.{block_executions|run_slices,step_slices,time_ns},
/// transitions.{aot_to_dynarmic,dynarmic_to_aot},
/// fallback_reasons.{lookup_miss,unhandled_opcode,icache_rejected,no_fallback_available},
/// icache.{clear_instruction_cache_calls,invalidate_cache_range_calls,
///         aot_range_rejects,permanent_aot_reject_events,jit_halt_cache_invalidation},
/// plus svc_calls, unresolved_import_traps, and histograms.
///
/// Benchmark comparison (drippu backlog #3) is not this snapshot. The stack
/// harness races the same guest fixture under JIT vs hybrid AOT and writes
/// `recomp_benchmark.json` (`kind=recomp_benchmark`), embedding per-mode
/// GetRecompExecutionMetrics() deltas. The bench ADD chain is STR/LDR-punctuated
/// so host -O3 cannot fold it to `x1 << 9`. The harness objdumps `block_bench`
/// and JudgeAotBenchDump fails on `shl $0x9` / `imul $512`; a jmp to a lower
/// PLT is not a loop. Named `recomp_store64@plt` is not the live pin (clang
/// PIC may omit it). Runtime host_mem load/store callback counts must be
/// 512 × iters. Override with $SUYU_RECOMP_BENCHMARK_JSON.
/// Default path: $SUYU_RECOMP_EXECUTION_JSON, else `{LogDir}/recomp_execution.json`
/// (Linux: ~/.local/share/suyu/log/recomp_execution.json unless portable `user/`).
/// WriteRecompExecutionJson keeps `std::filesystem::path` (no narrow `.string()`
/// ofstream) and publishes via dest+".tmp" then rename so a reader never sees a
/// torn file; an existing dest is replaced (RenameFile refuses overwrite).
struct RecompExecutionMetrics {
    static constexpr int kSchemaVersion = 1;

    u64 aot_block_executions{};
    u64 aot_time_ns{};
    u64 dynarmic_run_slices{};
    u64 dynarmic_step_slices{};
    u64 dynarmic_time_ns{};
    u64 aot_to_dynarmic{};
    u64 dynarmic_to_aot{};
    u64 fallback_lookup_miss{};
    u64 fallback_unhandled_opcode{};
    u64 fallback_icache_rejected{};
    u64 fallback_no_backend{};
    u64 unresolved_import_traps{};
    u64 svc_calls{};
    u64 clear_instruction_cache_calls{};
    u64 invalidate_cache_range_calls{};
    u64 aot_range_rejects{};
    u64 permanent_aot_reject_events{};
    u64 jit_halt_cache_invalidation{};
};

RecompExecutionMetrics GetRecompExecutionMetrics();
std::string FormatRecompExecutionJson();
std::filesystem::path DefaultRecompExecutionJsonPath();
bool WriteRecompExecutionJson(const std::filesystem::path& path = {});

/**
 * CPU backend that executes statically recompiled AArch64 rather than JITing
 * it.
 *
 * The point of routing this through ArmInterface is that everything above the
 * CPU in suyu - the HLE kernel, every service, the GPU - reaches the guest
 * only through this interface. Satisfying it means a recompiled game gets the
 * real HLE and GPU stack instead of the generated runtime's stub SVC handler,
 * which is what stops a recompiled commercial title from getting past its
 * first service call.
 *
 * Execution model: RunThread runs recompiled blocks back to back until the
 * generated code parks on an SVC (it writes the instruction's imm and the
 * resume PC into the context, then returns), at which point we hand control
 * back to the kernel with HaltReason::SupervisorCall exactly as the dynarmic
 * backend does. The kernel then reads arguments through GetSvcArguments,
 * dispatches the real service call, and writes results back through
 * SetSvcArguments before resuming us.
 */
class ArmRecomp final : public ArmInterface {
public:
    /// `process`, `exclusive_monitor` and `core_index` are only used to build a
    /// dynarmic JIT lazily, the first time a PC is reached that the static pass
    /// never covered (an indirect call into code no heuristic found). Without
    /// that fallback such a gap is terminal: the thread is suspended for a
    /// debugger that is not attached and the game hangs on a black screen with
    /// no forward progress.
    explicit ArmRecomp(System& system, bool uses_wall_clock, RecompLookupFn lookup,
                       Kernel::KProcess* process, DynarmicExclusiveMonitor* exclusive_monitor,
                       std::size_t core_index);
    ~ArmRecomp() override;

    HaltReason RunThread(Kernel::KThread* thread) override;
    HaltReason StepThread(Kernel::KThread* thread) override;

    void ClearInstructionCache() override;
    void InvalidateCacheRange(u64 addr, std::size_t size) override;

    Architecture GetArchitecture() const override {
        return Architecture::AArch64;
    }

    bool IsRecompBackend() const override {
        return true;
    }

    /// True until ClearInstructionCache permanently rejects Translate AOT.
    /// InvalidateCacheRange (loader RX protect) must leave this true.
    bool AllowsAot() const;

    void GetContext(Kernel::Svc::ThreadContext& ctx) const override;
    void SetContext(const Kernel::Svc::ThreadContext& ctx) override;
    void SetTpidrroEl0(u64 value) override;

    void GetSvcArguments(std::span<uint64_t, 8> args) const override;
    void SetSvcArguments(std::span<const uint64_t, 8> args) override;
    u32 GetSvcNumber() const override;

    void SignalInterrupt(Kernel::KThread* thread) override;

    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override;
    void RewindBreakpointInstruction() override;

private:
    /// Builds the JIT fallback if needed and marks this thread as running on
    /// it. Returns false when no JIT can be built (no process/monitor).
    bool EnterFallback();
    /// Runs the JIT fallback for one scheduling slice, syncing guest state in
    /// and back out, and returns to recompiled execution once the PC is covered
    /// again.
    HaltReason RunFallback(Kernel::KThread* thread);
    /// Same state sync as RunFallback, but steps a single guest instruction on
    /// the JIT. Used by StepThread so debugger single-step does not bypass the
    /// miss/unhandled fallback path that RunThread already uses.
    HaltReason StepFallback(Kernel::KThread* thread);

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Core
