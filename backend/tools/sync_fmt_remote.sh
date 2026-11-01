#!/bin/bash
# Sync formatted src/include to the Linux build server and verify.
# cli/ is required by the Makefile's cognitive-os-agent rule (client.h/main.c).
set -e
cd /d/AI/code/cognitive-os/backend
# third_party (cJSON/wasm3 sources, -I paths) and apps/web/index.html
# (web_ui.h generation rule) are both required by the Makefile.
tar czf /d/tmp/fmt.tgz src include tools tests cli Makefile third_party apps
scp -q -o BatchMode=yes /d/tmp/fmt.tgz root@101.43.16.207:/tmp/coa_fmt.tgz
ssh -o BatchMode=yes root@101.43.16.207 'bash -s' < /d/tmp/remote_build.sh > /d/tmp/remote_build.log 2>&1
echo "=== remote log tail ==="
tail -15 /d/tmp/remote_build.log
