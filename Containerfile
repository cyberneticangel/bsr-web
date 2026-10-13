# Big Scale Racing, ready to host: the game, its data and the online relay + leaderboard in one image.
#
# Needs the packed game data in web/data first (README, Build step 2: tools/pack.py); the engine is the committed
# web/bsr.js + web/bsr.wasm (rebuild with ./build.sh after changing src/). The image then holds the original game
# data, which is copyrighted: keep it in a private registry.
#
#   podman build -t bsr-web .                                  # or: docker build -f Containerfile -t bsr-web .
#   podman run -d --name bsr -p 8000:8000 -v bsr-data:/var/lib/bsr bsr-web
#
# Then open http://<host>:8000. Behind an HTTPS reverse proxy the game uses wss:// for /ws by itself; the proxy
# must pass WebSocket upgrades on /ws. The leaderboard is kept in the volume (/var/lib/bsr/leaderboard.json).
FROM docker.io/library/python:3.13-slim

RUN useradd --system --uid 10001 --no-create-home bsr \
 && mkdir -p /var/lib/bsr && chown bsr /var/lib/bsr
WORKDIR /srv/bsr
COPY tools/devserver.py tools/
COPY web/ web/
RUN test -f web/data/manifest.json \
 || { echo 'web/data/manifest.json is missing: run tools/pack.py first (see README, Build)' >&2; exit 1; }

USER bsr
ENV PYTHONUNBUFFERED=1
EXPOSE 8000
VOLUME /var/lib/bsr
CMD ["python3", "tools/devserver.py", "8000", "--host", "0.0.0.0", "--leaderboard", "/var/lib/bsr/leaderboard.json"]
