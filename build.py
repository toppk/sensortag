#!/usr/bin/python3
"""Build the CC2650 SensorTag firmware from TI's BLE-Stack 2.2.8 CCS projects, without CCS.

usage: build.py stack        BLE stack image (also regenerates the app/stack boundary)
       build.py app          application, FlashROM (needs a debugger to flash)
       build.py app --oad    application for over-the-air update (FlashOnly_OAD), plus its app-only .bin
       build.py oad          merge the OAD app and the stack into one update image, build/oad_merged.bin
       --stock               with app or oad: TI's SensorTag app as shipped in the SDK, without our changes
       build.py ti           TI's prebuilt SensorTag 1.50 as an update image, build/oad_ti_prebuilt.bin

Reads the projects' .project/.cproject files and runs the same compiler, linker,
XDC and TI helper steps CCS would. Output goes to build/<config>/.
"""
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
TI = os.environ.get("TI_ROOT", os.path.expanduser("~/apps/ti"))
SDK = f"{TI}/simplelink/ble_sdk_2_02_08_12"
TIRTOS = f"{TI}/tirtos_cc13xx_cc26xx_2_21_01_08"
CGT = f"{TI}/ti-cgt-arm_16.9.4.LTS"
XDC = f"{TI}/xdctools_3_32_00_06_core"
PROJ = f"{SDK}/examples/cc2650stk/sensortag/ccs"
BUILD = f"{HERE}/build"
CONFIG = f"{BUILD}/config"  # where the pre/post-build steps write, instead of the SDK's ccs/config
GENERATED = ("lib_linker.cmd", "ccs_compiler_defines.bcfg", "ccs_linker_defines.cmd")

VARS = {
    "CG_TOOL_ROOT": CGT,
    "CC26XXWARE": f"{TIRTOS}/products/cc26xxware_2_24_03_17272",
    "TI_RTOS_DRIVERS_BASE": f"{TIRTOS}/products/tidrivers_cc13xx_cc26xx_2_21_01_01/packages",
    "BIOS_BASE": f"{TIRTOS}/products/bios_6_46_01_38/packages",
    "SRC_BLE_CORE": f"{SDK}/src", "SRC_EX": f"{SDK}/src", "SRC_COMMON": f"{SDK}/src/components",
    "ROM": f"{SDK}/src/rom", "TOOLS_BLE": f"{SDK}/tools",
}
XDCPATH = ";".join([f"{TIRTOS}/packages", VARS["TI_RTOS_DRIVERS_BASE"], VARS["BIOS_BASE"],
                    f"{TIRTOS}/products/uia_2_01_00_01/packages"])

COMPILER = {  # CCS option id -> armcl flag; None means the value carries the flag
    "SILICON_VERSION": lambda v: f"-mv{v}", "CODE_STATE": "--code_state", "ABI": "--abi",
    "LITTLE_ENDIAN": lambda v: "-me", "OPT_LEVEL": lambda v: f"-O{v}", "OPT_FOR_SPEED": "--opt_for_speed",
    "INCLUDE_PATH": "--include_path", "C_DIALECT": lambda v: "--c99" if v == "C99" else "",
    "DEFINE": "--define", "DIAG_WARNING": "--diag_warning", "DISPLAY_ERROR_NUMBER": lambda v: "--display_error_number",
    "DIAG_SUPPRESS": "--diag_suppress", "DIAG_WRAP": "--diag_wrap", "CMD_FILE": "--cmd_file",
    "GEN_FUNC_SUBSECTIONS": "--gen_func_subsections",
}
LINKER = {
    "STACK_SIZE": "--stack_size", "HEAP_SIZE": "--heap_size", "MAP_FILE": "-m", "OUTPUT_FILE": "-o",
    "LIBRARY": "-l", "SEARCH_PATH": "-i", "DIAG_WRAP": "--diag_wrap", "DIAG_SUPPRESS": "--diag_suppress",
    "DISPLAY_ERROR_NUMBER": lambda v: "--display_error_number", "XML_LINK_INFO": "--xml_link_info",
    "ENTRY_POINT": "--entry_point", "UNUSED_SECTION_ELIMINATION": "--unused_section_elimination",
    "COMPRESS_DWARF": "--compress_dwarf", "DEFINE": "--define",
}


def expand(s, proj_dir, name):
    s = s.strip().strip('"')
    if s.startswith("file:/"):
        return s
    v = dict(VARS, ORG_PROJ_DIR=proj_dir, PROJECT_LOC=proj_dir, ProjName=name)
    s = re.sub(r"\$\{PARENT-(\d+)-(\w+)\}", lambda m: os.path.normpath(v[m[2]] + "/.." * int(m[1])), s)
    s = re.sub(r"^PARENT-(\d+)-(\w+)", lambda m: os.path.normpath(v[m[2]] + "/.." * int(m[1])), s)
    s = re.sub(r"\$\{(\w+)\}", lambda m: v.get(m[1], m[0]), s)
    s = re.sub(r"^(\w+)/", lambda m: v[m[1]] + "/" if m[1] in v else m[0], s)
    s = os.path.normpath(s) if s.startswith("/") else s
    for f in GENERATED:
        s = s.replace(f"{PROJ}/config/{f}", f"{CONFIG}/{f}")
    return s


def project(part, config, stock=False):
    """Linked sources and tool options for one CCS project configuration."""
    d = f"{PROJ}/{part}"
    name = f"sensortag_cc2650stk_{part}"
    links = {l.findtext("name"): expand(l.findtext("locationURI"), d, name)
             for l in ET.parse(f"{d}/.project").iter("link") if l.findtext("type") == "1"}
    cfg = next(c for c in ET.parse(f"{d}/.cproject").iter("configuration") if c.get("name") == config)
    excluded = set()
    for e in cfg.iter("entry"):
        excluded |= set((e.get("excluding") or "").split("|"))
    files = [p for n, p in links.items() if n not in excluded]
    files = [f"{HERE}/src/{part}/{os.path.basename(p)}" if not stock and os.path.exists(f"{HERE}/src/{part}/{os.path.basename(p)}")
             else p for p in files]  # our modified copies replace the SDK's
    tools = {}
    for tool in cfg.iter("tool"):
        opts = tools.setdefault(tool.get("name"), {})
        for o in tool.iter("option"):
            key = o.get("superClass").split(".")[-1]
            vals = [v.get("value") for v in o.iter("listOptionValue")]
            opts[key] = [expand(x, d, name) for x in vals] if vals else enum(expand(o.get("value") or "", d, name))
    return name, files, tools, cfg


def enum(v):
    return v.split(".")[-1] if v.startswith("com.ti.") else v


def flags(table, opts):
    out = []
    for key, val in sorted(opts.items(), key=lambda kv: kv[0] != "SEARCH_PATH"):  # -i before -l
        f = table.get(key)
        if f is None or val in ("", []):
            continue
        for v in val if isinstance(val, list) else [val]:
            out.append(f(v) if callable(f) else f"{f}={v}" if f.startswith("--") else f"{f}{v}")
    return [x for x in out if x]


def arch(cflags):
    return [f for f in cflags if f.startswith(("-mv", "--code_state", "--abi", "-me"))]


def link_order(lflags, inputs):
    """Options, then objects and .cmd files, then libraries, as CCS orders them."""
    libs = [f for f in lflags if f.startswith("-l")]
    return [f for f in lflags if f not in libs] + inputs + libs


def run(cmd, cwd):
    print("  " + " ".join(os.path.relpath(c, cwd) if c.startswith(HERE) else c for c in cmd)[:200], flush=True)
    if subprocess.run(cmd, cwd=cwd).returncode:
        sys.exit(f"build: {os.path.basename(cmd[0])} failed")


def armcl(cflags, src, out, cwd):
    obj = f"{out}/{os.path.splitext(os.path.basename(src))[0]}.obj"
    run([f"{CGT}/bin/armcl", *cflags, "-g", f"--obj_directory={out}", src], cwd)
    return obj


def case_shim(cflags, tag, extra=()):
    """Resolve #includes the way Windows would (first include dir with a case-insensitive
    match), linking each name whose answer differs from Linux's into a dir searched first."""
    shim = f"{BUILD}/caseshim/{tag}"
    shutil.rmtree(shim, ignore_errors=True)
    os.makedirs(shim)
    dirs = [f.split("=", 1)[1] for f in cflags if f.startswith("--include_path=")] + list(extra)
    dirs = [d for d in dirs if os.path.isdir(d)]

    def ci(d, rel):  # rel under d, matching each part ignoring case
        for part in rel.split("/"):
            if not os.path.isdir(d):
                return None
            d = next((f"{d}/{n}" for n in sorted(os.listdir(d)) if n.lower() == part.lower()), None)
            if d is None:
                return None
        return d

    wanted = set()
    for root in (SDK, TIRTOS):
        for d, _, fs in os.walk(root):
            for f in fs:
                if f.endswith((".c", ".h")):
                    with open(f"{d}/{f}", errors="replace") as fh:
                        wanted |= set(re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', fh.read(), re.M))
    for w in sorted(x for x in wanted if ".." not in x and not x.startswith("/")):
        win = next((r for d in dirs if (r := ci(d, w)) and os.path.isfile(r)), None)
        lin = next((f"{d}/{w}" for d in dirs if os.path.isfile(f"{d}/{w}")), None)
        if win and win != lin:
            os.makedirs(os.path.dirname(f"{shim}/{w}"), exist_ok=True)
            os.symlink(win, f"{shim}/{w}")
    return [f"--include_path={shim}"]


def seed_config():
    os.makedirs(CONFIG, exist_ok=True)
    for f in GENERATED[1:]:
        if not os.path.exists(f"{CONFIG}/{f}"):
            sys.exit("build: run 'build.py stack' first; the app needs its boundary")


def build_stack():
    seed_config()
    name, files, tools, _ = project("stack", "FlashROM")
    out = f"{BUILD}/stack"
    os.makedirs(out, exist_ok=True)
    run([sys.executable, f"{HERE}/tools/lib_search.py", f"{PROJ}/stack/build_config.opt",
         f"{SDK}/tools/lib_search/params_split_cc2640.xml", f"{SDK}/blelib", f"{CONFIG}/lib_linker.cmd"], out)
    cflags = flags(COMPILER, tools["ARM Compiler"])
    cflags = case_shim(cflags, "stack") + cflags
    objs = [armcl(cflags, f, out, out) for f in files if f.endswith(".c")]
    cmds = [f for f in files if f.endswith(".cmd") and not f.endswith("lib_linker.cmd")]
    lflags = flags(LINKER, tools["ARM Linker"])
    run([f"{CGT}/bin/armcl", *arch(cflags), "-g", "-z", "--reread_libs", "--rom_model", "--warn_sections",
         *link_order(lflags, objs + cmds)], out)
    run([f"{CGT}/bin/armhex", "-order", "MS", "--memwidth=8", "--romwidth=8", "--intel",
         "-o", f"{name}.hex", f"{name}.out"], out)
    run([sys.executable, f"{HERE}/tools/frontier.py", "ccs", f"{out}/{name}_linkInfo.xml",
         f"{CONFIG}/ccs_compiler_defines.bcfg", f"{CONFIG}/ccs_linker_defines.cmd"], out)
    print(f"{out}/{name}.hex")


def build_app(oad, stock):
    seed_config()
    config = "FlashOnly_OAD" if oad else "FlashROM"
    name, files, tools, _ = project("app", config, stock)
    out = f"{BUILD}/{'app_oad' if oad else 'app'}{'_stock' if stock else ''}"
    os.makedirs(out, exist_ok=True)
    cflags = flags(COMPILER, tools["ARM Compiler"]) + ([] if stock else build_id() + APP_DEFINES)
    x = tools["XDCtools"]
    cfg = next(f for f in files if f.endswith(".cfg"))
    run([f"{XDC}/xs", f"--xdcpath={XDCPATH}", "xdc.tools.configuro", "-o", "configPkg",
         "-t", "ti.targets.arm.elf.M3", "-p", x["PLATFORM"],
         "-r", "release", "-c", CGT, "--compileOptions", " ".join(cflags),
         *(["--cfgArgs", x["CFG_ARGS"]] if x.get("CFG_ARGS") else []), cfg], out)
    cflags.append(f"--cmd_file={out}/configPkg/compiler.opt")
    with open(f"{out}/configPkg/compiler.opt") as f:
        xdc_dirs = [os.path.normpath(d) for d in re.findall(r'-I"?([^" ]+)"?', f.read())]
    cflags = case_shim(cflags, "app", xdc_dirs) + cflags
    objs = [armcl(cflags, f, out, out) for f in files if f.endswith(".c")]
    cmds = [f for f in files if f.endswith(".cmd")]
    lflags = flags(LINKER, tools["ARM Linker"])
    run([f"{CGT}/bin/armcl", *arch(cflags), "-g", "-z", "--reread_libs", "--rom_model", "--warn_sections",
         *link_order(lflags, objs + cmds + [f"{out}/configPkg/linker.cmd"])], out)
    run([f"{CGT}/bin/armhex", "-order", "MS", "--memwidth=8", "--romwidth=8", "--intel",
         "-o", f"{name}.hex", f"{name}.out"], out)
    print(f"{out}/{name}.hex")
    if oad:
        oad_image([f"{out}/{name}.hex"], f"{out}/{name}_oad")


# Room for our beacon in the OAD app: drop the IR temperature service (no TMP007 on
# Rev 1.5.1 boards) and the register service (raw I2C access for any connected client)
APP_DEFINES = ["--define=EXCLUDE_TMP", "--define=EXCLUDE_REG"]


def build_id():
    """Defines that let the app's info beacon say which source it was built from."""
    git = lambda *a: subprocess.run(["git", "-C", HERE, *a], capture_output=True, text=True).stdout.strip()
    rev = git("rev-parse", "--short=8", "HEAD") or "0"
    dirty = 1 if git("status", "--porcelain", "--", "src", "tools", "build.py") else 0
    ver = re.search(r"ble_sdk_(\d+)_(\d+)_(\d+)", SDK)
    with open(f"{SDK}/src/examples/sensortag/cc26xx/app/sensortag_revision.h") as f:
        fw = re.search(r"#define\s+FW_VERSION\s+(\d+)\.(\d\d)", f.read())
    return [f"--define=BUILD_GIT=0x{rev}", f"--define=BUILD_DIRTY={dirty}", f"--define=FW_VERSION_X100={fw[1]}{fw[2]}",
            f"--define=BLE_SDK_VERSION=0x{int(ver[1]):02x}{int(ver[2]):02x}{int(ver[3]):02x}"]


def oad_image(hexes, dest):
    run([sys.executable, f"{HERE}/tools/oad_image_tool.py", *hexes, "-t", "offchip", "-i", "app", "--imgVer", "0",
         "-m", "0x1000", "-r", "0x1000:", "-q", "-o", f"{dest}.hex", "-ob", f"{dest}.bin"], BUILD)
    print(f"{dest}.bin")


def main():
    a = sys.argv[1:]
    stock = "--stock" in a
    a = [x for x in a if x != "--stock"]
    if a == ["stack"] and not stock:
        build_stack()
    elif a in (["app"], ["app", "--oad"]):
        build_app("--oad" in a, stock)
    elif a == ["ti"] and not stock:
        from intelhex import IntelHex
        ih = IntelHex(f"{SDK}/examples/hex/cc2650stk_sensortag_rel.hex")[0x1000:0x1F000]  # app + stack, no BIM pages
        ih.write_hex_file(f"{BUILD}/ti_prebuilt_app_stack.hex")
        oad_image([f"{BUILD}/ti_prebuilt_app_stack.hex"], f"{BUILD}/oad_ti_prebuilt")
    elif a == ["oad"]:
        sfx = "_stock" if stock else ""
        hexes = [f"{BUILD}/app_oad{sfx}/sensortag_cc2650stk_app.hex", f"{BUILD}/stack/sensortag_cc2650stk_stack.hex"]
        for h in hexes:
            if not os.path.exists(h):
                sys.exit(f"build: {os.path.relpath(h, HERE)} missing; run build.py stack and app --oad{' --stock' * stock} first")
        oad_image(hexes, f"{BUILD}/oad_merged{sfx}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
