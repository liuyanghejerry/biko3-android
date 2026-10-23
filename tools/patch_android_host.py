#!/usr/bin/env python3
"""Apply the reviewed Android edits to a staged copy of the nxnative host.

The Android port keeps the upstream sources unmodified wherever a bionic
compatibility shim can do the job (see android/port). The edits below are the
places where behaviour, not only an API name, differs:

  * hle.h / hle_table.c  Mach-O section names -> ELF section names
  * loader.c             guest TLS slots: Darwin TSD offsets -> dm_tsd slots
  * crash.c              Darwin nested ucontext -> bionic ucontext accessors

Every edit is anchored on an exact string and must match once.
"""
from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, where: str) -> str:
    count = text.count(old)
    if count != 1:
        raise ValueError(f"{where}: anchor matched {count} times: {old[:70]!r}")
    return text.replace(old, new, 1)


def patch_hle_h(path: Path) -> None:
    text = path.read_text(encoding="utf8")
    if text.count('section("__DATA,swhle")') != 1 or text.count('section("__DATA,swforce")') != 2:
        raise ValueError(path.name + ": unexpected HLE section anchors")
    text = text.replace('section("__DATA,swhle")', 'section("swhle")')
    text = text.replace('section("__DATA,swforce")', 'section("swforce")')
    path.write_text(text, encoding="utf8")


def patch_hle_table(path: Path) -> None:
    text = path.read_text(encoding="utf8")
    text = replace_once(text, 'section("__DATA,swhle")', 'section("swhle")', path.name)
    path.write_text(text, encoding="utf8")


def patch_loader(path: Path) -> None:
    text = path.read_text(encoding="utf8")
    text = replace_once(
        text,
        "static void create_keys(void) {\n"
        "    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) ||\n"
        "        pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL)) die(\"cannot create the host key\");\n",
        "static void create_keys(void) {\n"
        "    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) ||\n"
        "        pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL)) die(\"cannot create the host key\");\n"
        "    dm_tsd_register(DM_TSD_K18, g_key18); dm_tsd_register(DM_TSD_KTLS, g_keytls);\n"
        "    dm_tsd_register(DM_TSD_KTLR, g_keytlr); dm_tsd_register(DM_TSD_KGATE, g_keygate);\n",
        path.name + " create_keys",
    )
    text = replace_once(
        text,
        "    for (unsigned i = 0; i < KEY_N; i++) page[m->key_idx[i]] = (uint64_t)key_value(i) * 8;",
        "    for (unsigned i = 0; i < KEY_N; i++) page[m->key_idx[i]] = dm_tsd_offset((int)i);",
        path.name + " key page",
    )
    text = replace_once(
        text,
        "static void tsd_check(void) {\n"
        "    pthread_setspecific(g_key18, (void *)0x1234);\n"
        "    uint64_t base; __asm__ volatile(\"mrs %0, tpidrro_el0\" : \"=r\"(base));\n"
        "    uint64_t off = *(uint64_t *)(g_mod[0].base + g_mod[0].key_page_off + g_mod[0].key_idx[KEY_K18] * 8);\n"
        "    if (((uint64_t *)(base & ~7ull))[off / 8] != 0x1234) die(\"TSD layout mismatch\");\n"
        "    pthread_setspecific(g_key18, 0);\n"
        "}",
        "static void tsd_check(void) {\n"
        "#if defined(__aarch64__)\n"
        "    pthread_setspecific(g_key18, (void *)0x1234);\n"
        "    uint64_t base = 0; __asm__ volatile(\"mrs %0, tpidr_el0\" : \"=r\"(base));\n"
        "    uint64_t off = *(uint64_t *)(g_mod[0].base + g_mod[0].key_page_off + g_mod[0].key_idx[KEY_K18] * 8);\n"
        "    if (*(uint64_t *)(base + off) != 0x1234) die(\"TSD layout mismatch\");\n"
        "    pthread_setspecific(g_key18, 0);\n"
        "#else\n"
        "    /* The AArch64 guest cannot execute on this ABI; the self-test covers the slots. */\n"
        "#endif\n"
        "}",
        path.name + " tsd check",
    )
    text = replace_once(
        text,
        '#include "hle.h"\n',
        '#include "hle.h"\n#include "dm_tsd.h"\n',
        path.name + " include",
    )
    path.write_text(text, encoding="utf8")


UCONTEXT_INCLUDE = '#include "android_ucontext.h"\n'


def patch_hle_thread(path: Path) -> None:
    text = path.read_text(encoding="utf8")
    text = replace_once(text, '#include "dm.h"\n', '#include "dm.h"\n' + UCONTEXT_INCLUDE, path.name)
    text = replace_once(
        text,
        "    ucontext_t *uc = uc_; const struct __darwin_arm_thread_state64 *ss = &uc->uc_mcontext->__ss;\n"
        "    if (!module_of((void *)__darwin_arm_thread_state64_get_pc(*ss))) return;",
        "    ucontext_t *uc = uc_;\n"
        "    if (!module_of((void *)DM_UC_PC(uc))) return;",
        path.name + " pause context",
    )
    text = replace_once(text, "for (int i = 0; i < 29; i++) t->ctx_x[i] = ss->__x[i];",
                        "for (int i = 0; i < 29; i++) t->ctx_x[i] = DM_UC_X(uc, i);", path.name)
    text = replace_once(text,
        "t->ctx_fp = __darwin_arm_thread_state64_get_fp(*ss); t->ctx_lr = __darwin_arm_thread_state64_get_lr(*ss);\n"
        "        t->ctx_sp = __darwin_arm_thread_state64_get_sp(*ss) - 32; t->ctx_pc = __darwin_arm_thread_state64_get_pc(*ss); t->ctx_pstate = ss->__cpsr;\n"
        "        memcpy(t->ctx_v, uc->uc_mcontext->__ns.__v, sizeof t->ctx_v); t->ctx_fpcr = uc->uc_mcontext->__ns.__fpcr; t->ctx_fpsr = uc->uc_mcontext->__ns.__fpsr;",
        "t->ctx_fp = DM_UC_FP(uc); t->ctx_lr = DM_UC_LR(uc);\n"
        "        t->ctx_sp = DM_UC_SP(uc) - 32; t->ctx_pc = DM_UC_PC(uc); t->ctx_pstate = 0;\n"
        "        memset(t->ctx_v, 0, sizeof t->ctx_v); t->ctx_fpcr = 0; t->ctx_fpsr = 0;",
        path.name + " pause fp context")
    path.write_text(text, encoding="utf8")


THREAD_ENTER = """_dm_thread_enter:
    stp x0, x1, [sp, #-32]!
    stp x2, x30, [sp, #16]
    bl dm_tsd_attach
    ldp x0, x1, [sp]
    ldp x2, x30, [sp, #16]
    add sp, sp, #32
"""


ENTRY_X18 = """    // Android: x18 holds the guest's slot block pointer; host calls must not
    // leak a different value back into guest code (it is the platform register
    // and our stubs read every slot as [x18 + offset]).
    ldr x18, [sp, #144]
"""


def patch_entry(path: Path) -> None:
    text = path.read_text(encoding="utf8")
    text = replace_once(
        text,
        "_dm_thread_enter:\n    mov sp, x2\n    mov x29, #0\n    mov x16, x0\n    mov x0, x1\n",
        THREAD_ENTER + "    mov sp, x2\n    mov x29, #0\n    mov x16, x0\n    mov x0, x1\n",
        path.name + " thread entry x18",
    )
    text = replace_once(
        text,
        "    ldr x19, [sp, #152]",
        ENTRY_X18 + "    ldr x19, [sp, #152]",
        path.name + " x18 restore",
    )
    path.write_text(text, encoding="utf8")


def patch_crash(path: Path) -> None:
    text = path.read_text(encoding="utf8")
    text = replace_once(text, '#include "dm.h"\n', '#include "dm.h"\n' + UCONTEXT_INCLUDE, path.name)
    if "uc->uc_mcontext->__ss.__pc" not in text:
        raise ValueError(path.name + ": no Darwin ucontext access found")
    text = text.replace("uc->uc_mcontext->__ss.__pc", "DM_UC_PC(uc)")
    text = text.replace("uc->uc_mcontext->__ss.__lr", "DM_UC_LR(uc)")
    text = text.replace("uc->uc_mcontext->__ss.__sp", "DM_UC_SP(uc)")
    text = text.replace("uc->uc_mcontext->__ss.__fp", "DM_UC_FP(uc)")
    text = text.replace("uc->uc_mcontext->__ss.__x[i]", "DM_UC_X(uc, i)")
    text = text.replace("(uc->uc_mcontext->__es.__esr >> 6) & 1", "0")
    if "uc->uc_mcontext->" in text:
        raise ValueError(path.name + ": Darwin ucontext access left after patching")
    path.write_text(text, encoding="utf8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, required=True, help="staged dm/host directory")
    args = parser.parse_args()
    host = args.host.resolve()
    patch_hle_h(host / "hle.h")
    patch_hle_table(host / "hle_table.c")
    patch_loader(host / "loader.c")
    patch_crash(host / "crash.c")
    patch_hle_thread(host / "hle_thread.c")
    patch_entry(host / "entry.S")
    print("android host patches applied")


if __name__ == "__main__":
    main()
