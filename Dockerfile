FROM debian:stable-slim

RUN apt-get update \
 && apt-get install build-essential -y \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

WORKDIR inotify-info

COPY . .

RUN make

FROM debian:stable-slim
COPY --from=0 /inotify-info/_release/inotify-info /bin/inotify-info

CMD /bin/inotify-info
