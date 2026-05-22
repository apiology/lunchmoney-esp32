Import("env")
from pathlib import Path

libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "lvgl"
blend = libdeps / "src/draw/sw/blend"

for asm in blend.rglob("*.S"):
    disabled = asm.with_suffix(asm.suffix + ".disabled")
    if asm.exists() and not disabled.exists():
        asm.rename(disabled)
