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
# Important addition: compiled hash.c as part of our broker bundle.
BROKER_SRCS = src/main.c src/ts_queue.c src/client_manager.c src/worker.c src/pubsub.c src/auth.c src/tls.c src/enroll.c src/cli.c src/heartbeat.c src/db.c src/config.c src/tokenizer.c src/rbac.c src/hash.c
AGENT_SRCS = src/agent.c src/agent_config.c src/tokenizer.c

# --- Object Files ---
BROKER_OBJS = $(BROKER_SRCS:.c=.o)
AGENT_OBJS = $(AGENT_SRCS:.c=.o)

# --- Targets ---
all: $(BROKER_BIN) $(AGENT_BIN)

$(BROKER_BIN): $(BROKER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS_BROKER)

$(AGENT_BIN): $(AGENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS_AGENT)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(BROKER_OBJS) $(AGENT_OBJS) $(BROKER_BIN) $(AGENT_BIN)

.PHONY: all clean
