#!/bin/bash
# Sync formatted src/include to the Linux build server and verify.
set -e
cd /d/AI/code/cognitive-os/backend
tar czf /d/tmp/coa_fmt.tgz src include Makefile
scp -q -o BatchMode=yes /d/tmp/coa_fmt.tgz root@101.43.16.207:/tmp/coa_fmt.tgz
ssh -o BatchMode=yes root@101.43.16.207 'bash -s' < /d/tmp/remote_build.sh > /d/tmp/remote_build.log 2>&1
echo "=== remote log tail ==="
tail -15 /d/tmp/remote_build.log
