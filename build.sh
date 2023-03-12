#!/usr/bin/env bash
gcc -o xul.so xul.c $(yed --print-cflags --print-ldflags)
