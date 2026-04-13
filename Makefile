# ========================
# 编译器与参数
# ========================
CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2 -pthread
LDFLAGS =
LDLIBS = -lmysqlclient -lhiredis

# ========================
# 目录
# ========================
SRC_DIR = src
BUILD_DIR = build
INCLUDE_DIR = include

# 目标程序（和 main.cpp 同目录）
TARGET = $(SRC_DIR)/chat_server

# ========================
# 源文件
# ========================
SRCS = $(shell find $(SRC_DIR) -name "*.cpp")

# ========================
# 目标文件（放 build 目录）
# ========================
OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))

# ========================
# 头文件路径
# ========================
INCLUDES = -I$(INCLUDE_DIR) -I$(INCLUDE_DIR)/common -I$(INCLUDE_DIR)/pool

# ========================
# 默认目标
# ========================
all: $(TARGET)

# ========================
# 链接
# 注意：库要放在对象文件后面
# ========================
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

# ========================
# 编译规则
# ========================
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ========================
# 清理
# ========================
clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# ========================
# 重新编译
# ========================
rebuild: clean all

.PHONY: all clean rebuild