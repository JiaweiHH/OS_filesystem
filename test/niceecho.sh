#!/bin/bash

NONE="\e[0m"
GRAY="\e[1;37m"
RED="\e[1;31m"
YELLOW="\e[1;33m"
GREEN="\e[1;32m"

echoinfo() {
  echo -e ${GRAY}$*${NONE}
}

echowarning() {
  echo -e ${YELLOW}$*${NONE}
}

echoerr() {
  echo -e ${RED}"\n"$*"\n"${NONE}
}

echosucc() {
  echo -e ${GREEN}"\n"$*"\n"${NONE}
}