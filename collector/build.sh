#!/bin/sh
set -eu
cd "$(dirname "$0")"
clang -arch x86_64 -mmacosx-version-min=11.0 -std=c11 -O2 -Wall -Wextra -Werror \
    wakeupctl.c -framework IOKit -framework CoreFoundation -o wakeupctl-x86_64
printf '%s\n' 'Built wakeupctl-x86_64 for AMD/Intel x86_64; no hardware command was sent.'
