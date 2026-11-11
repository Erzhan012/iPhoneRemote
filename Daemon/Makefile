TARGET := iphone:clang:latest:15.0
ARCHS = arm64
THEOS_PACKAGE_SCHEME = rootless

include $(THEOS)/makefiles/common.mk

TOOL_NAME = iphoneremoted
iphoneremoted_FILES = main.cpp DeviceInfo.cpp
iphoneremoted_CXXFLAGS = -std=c++17 -O2
iphoneremoted_FRAMEWORKS = CoreFoundation IOKit
iphoneremoted_INSTALL_PATH = /usr/local/bin
iphoneremoted_CODESIGN_FLAGS = -Sent.plist

include $(THEOS_MAKE_PATH)/tool.mk
