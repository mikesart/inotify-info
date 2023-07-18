FROM debian

RUN apt-get update \
 && apt-get install build-essential -y \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

WORKDIR inotify-info

COPY . .

RUN make

CMD ./_release/inotify-info