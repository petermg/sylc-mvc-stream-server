# HTTP API

Base URL example:

```text
http://server-address:8097
```

When API-token authentication is enabled, protected requests accept either:

```http
X-SyLC-Token: TOKEN
```

or:

```http
Authorization: Bearer TOKEN
```

## Public endpoints

- `GET /api/health`
- `GET /api/setup/status`
- `POST /api/setup` — available only until the first library is created
- `GET /hls/{session-id}/stream.m3u8`
- `GET /hls/{session-id}/{segment}`
- static web UI assets

Directory browsing and library path testing are available without a token only while initial setup is incomplete.

## Media Libraries

- `GET /api/libraries`
- `POST /api/libraries`
- `PUT /api/libraries/{library-id}`
- `DELETE /api/libraries/{library-id}`
- `POST /api/libraries/{library-id}/rescan`
- `POST /api/libraries/test`
- `GET /api/filesystem/directories?path=/mnt/media`

Example create body:

```json
{
  "name": "3D Movies",
  "path": "/mnt/media/3d-movies",
  "recursive": true,
  "enabled": true
}
```

## Authentication

- `GET /api/auth/check`
- `POST /api/settings/token`

Token-update body:

```json
{"apiToken":"new-token"}
```

An empty token disables API authentication.

## Catalog and sessions

- `GET /api/media?refresh=1`
- `POST /api/media/probe`
- `GET /api/sessions/current`
- `GET /api/sessions/current/report`
- `POST /api/sessions`
- `POST /api/sessions/current/seek`
- `POST /api/sessions/current/stop`
- `POST /api/sessions/cleanup`

Session-create example:

```json
{
  "mediaId": "...",
  "mode": "anaglyph-dubois",
  "audioStream": 0,
  "swapEyes": false,
  "startSeconds": 1200
}
```

Supported modes:

```text
half-sbs
full-sbs
half-ou
full-ou
left-eye
right-eye
anaglyph-color
anaglyph-dubois
passive-rows-left-top
passive-rows-right-top
```
