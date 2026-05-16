#!/usr/bin/env bash
set -euo pipefail

LOCAL_PORT="${Q2_MONGO_TUNNEL_LOCAL_PORT:-27018}"
REMOTE_HOST="${Q2_MONGO_TUNNEL_REMOTE_HOST:-127.0.0.1}"
REMOTE_PORT="${Q2_MONGO_TUNNEL_REMOTE_PORT:-27017}"
SSH_HOST="${Q2_MONGO_TUNNEL_SSH_HOST:-golgong.com}"
SSH_PORT="${Q2_MONGO_TUNNEL_SSH_PORT:-10022}"
SSH_USER="${Q2_MONGO_TUNNEL_SSH_USER:-orange}"

export SSHPASS="${Q2_MONGO_TUNNEL_PASSWORD:-${SSHPASS:-Qjawlsdk12!@}}"

exec sshpass -e ssh \
  -N \
  -o ExitOnForwardFailure=yes \
  -o ServerAliveInterval=30 \
  -o ServerAliveCountMax=3 \
  -o StrictHostKeyChecking=no \
  -p "$SSH_PORT" \
  -L "127.0.0.1:${LOCAL_PORT}:${REMOTE_HOST}:${REMOTE_PORT}" \
  "${SSH_USER}@${SSH_HOST}"
