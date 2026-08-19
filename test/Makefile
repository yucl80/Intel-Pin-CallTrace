# 演示用测试程序，普通 gcc 编译即可，不需要 Pin。
CC      := gcc
CXX     := g++
CFLAGS  := -O0 -g -Wall

all: testapp testcpp

testapp: testapp.c
	$(CC) $(CFLAGS) -o $@ $^

testcpp: testcpp.cpp
	$(CXX) $(CFLAGS) -o $@ $^

clean:
	rm -f testapp testcpp
