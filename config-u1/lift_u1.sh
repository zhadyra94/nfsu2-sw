#!/usr/bin/env bash
set -uo pipefail
XBE=/home/madik/workspace/nfs-ios/dl/u1/default.xbe
GEN=/home/madik/workspace/nfs-ios/gen-u1
U1=/home/madik/workspace/nfs-ios/u1
cd $U1/xboxrecomp
echo "== disasm"; time python3 -m tools.disasm "$XBE" --seed-functions $U1/config/seed_functions.json --analysis-json $U1/config/default_analysis.json || echo "DISASM FAILED $?"
echo "== func_id"; time python3 -m tools.func_id "$XBE" || echo "FUNC_ID FAILED $?"
echo "== abi"; time python3 -m tools.abi_analysis "$XBE" || echo "ABI FAILED $?"
mkdir -p $GEN; rm -f $GEN/recomp_types.h
echo "== recomp"; time python3 -m tools.recomp "$XBE" --game-name "NFS Underground" --all --split 250 --gen-dir $GEN --exclude-manual /home/madik/workspace/nfs-ios/nfsu1-sw/src/recomp_manual.c --mmio-sections DSOUND,XPP || echo "RECOMP FAILED $?"
echo DONE
