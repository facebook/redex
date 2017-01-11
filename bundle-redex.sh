#!/bin/bash

tar czf redex.tar.gz redex-all redex.py pyredex/*.py
cat selfextract.sh redex.tar.gz > redex
chmod +x redex
