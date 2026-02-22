# --- Compiler Settings ---
CC = gcc
CFLAGS = -Wall -g

# --- Libraries ---
LIBS_BROKER = -lpthread -lssl -lcrypto -lsqlite3
LIBS_AGENT = -lssl -lcrypto -lpthread

# --- Executable Names ---
BROKER_BIN = message_broker
AGENT_BIN = agent

# --- Source Files ---
BROKER_SRCS = src/main.c src/ts_queue.c src/client_manager.c src/worker.c src/pubsub.c src/auth.c src/tls.c src/enroll.c src/cli.c src/heartbeat.c src/db.c src/config.c
AGENT_SRCS = src/agent.c src/agent_config.c

# --- Object Files ---
BROKER_OBJS = $(BROKER_SRCS:.c=.o)
AGENT_OBJS = $(AGENT_SRCS:.c=.o)

# --- Targets ---

# 'all' is the default target
all: $(BROKER_BIN) $(AGENT_BIN)

# Link the Broker executable
$(BROKER_BIN): $(BROKER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS_BROKER)

# Link the Agent executable
$(AGENT_BIN): $(AGENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS_AGENT)

# Compile .c source files into .o object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean target to remove compiled binaries and object files
clean:
	rm -f $(BROKER_OBJS) $(AGENT_OBJS) $(BROKER_BIN) $(AGENT_BIN)

# Phony targets aren't real files
.PHONY: all clean
