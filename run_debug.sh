#!/bin/bash
cd /home/gilbert/DEV/fbneo-launcher/build
./fbneo-launcher 2>&1 | tee debug_output.log
