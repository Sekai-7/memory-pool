FROM sekai7/cpp_env:v3

RUN apt-get update && apt-get install -y \
    gdb \
    valgrind \
    strace \
    vim