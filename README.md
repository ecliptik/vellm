# vellm

A port of [karpathy/llama2.c](https://github.com/karpathy/llama2.c) to MS-DOS
6.22, targeting a Pentium Overdrive 83 MHz system (PODP5V83, 48 MB RAM,
CF-to-IDE, CWSDPMI r7 for DPMI). The name is pronounced *vellum*.

The primary deliverable is a statically-linked `vellm.exe` that loads an
int8-quantized TinyStories checkpoint and generates text on period hardware.

**Status: work in progress.** Phase 0 scaffolding is underway; see
[`PLAN.md`](./PLAN.md) for the phased roadmap and [`CLAUDE.md`](./CLAUDE.md)
for the day-to-day operating guide.

This README is a stub; it will be expanded in Phase 5 with a full build /
install / hardware matrix.
