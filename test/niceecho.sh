#!/bin/bash

NONE="\e[0m"
GRAY="\e[1;37m"
RED="\e[1;31m"
YELLOW="\e[1;33m"
GREEN="\e[1;32m"

echoinfo() {
  echo -e ${GRAY}$*${NONE}

  if [ $logfile != '' ]; then
    echo -e $* >> ${logfile}
  fi
}

echowarning() {
  echo -e ${YELLOW}$*${NONE}

  if [ $logfile != '' ]; then
    echo -e $* >> ${logfile}
  fi
}

echoerr() {
  echo -e ${RED}"\n"$*"\n"${NONE}

  if [ $logfile != '' ]; then
    echo -e $* >> ${logfile}
  fi
}

echosucc() {
  echo -e ${GREEN}"\n"$*"\n"${NONE}

  if [ $logfile != '' ]; then
    echo -e $* >> ${logfile}
  fi
}