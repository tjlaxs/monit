#!/bin/sh

export PATH=$PATH:.

StrTest && \
TimeTest && \
SystemTest && \
ListTest && \
LinkTest && \
StringBufferTest && \
DirTest && \
InputStreamTest && \
OutputStreamTest && \
FileTest && \
ExceptionTest && \
NetTest && \
CommandTest
