#! /bin/bash

# Shutdown all domains

xl list | sed -rn s/^\w+\s+([0-9]+).*/\1/p
