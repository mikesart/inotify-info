FROM alpine

# zig is installed from the upstream tarball, because:
# - as of writing, alpine has zig only in testing (which is cumbersome to use)
# - apk get zig pulls in libllvm, which is huge.
#
# Upstream tarball is statically linked, making it small and convenient to use.
RUN apk add make \
 && wget https://ziglang.org/download/0.12.0/zig-linux-$(uname -m)-0.12.0.tar.xz \
 && tar -xJf zig-linux-*.tar.xz \
 && rm zig-linux-*.xz \
 && mv zig-linux-* zig

WORKDIR inotify-info

COPY . .

RUN CC="/zig/zig cc -target $(uname -m)-linux-musl" \
    CXX="/zig/zig c++ -target $(uname -m)-linux-musl" \
    make

FROM scratch
COPY --from=0 /inotify-info/_release/inotify-info /inotify-info

ENTRYPOINT ["/inotify-info"]
