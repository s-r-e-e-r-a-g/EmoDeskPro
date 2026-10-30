# PlatformIO pre-build hook: rebuild web_ui.h from web/index.html
Import("env")  # noqa: F821
import subprocess, sys, os
root = env["PROJECT_DIR"]  # noqa: F821
subprocess.check_call([sys.executable, os.path.join(root, "tools", "build_web.py")])
