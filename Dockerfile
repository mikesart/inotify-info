FROM alpine:edge

RUN apk add make zig git

WORKDIR inotify-info

COPY . .

RUN CC="zig cc -target $(uname -m)-linux-musl" \
    CXX="zig c++ -target $(uname -m)-linux-musl" \
    make VERBOSE=1

FROM scratch
COPY --from=0 /inotify-info/_release/inotify-info /inotify-info

ENTRYPOINT ["/inotify-info"]
