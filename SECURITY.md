# Security

## Deployment model

SyLC MVC Stream Server is designed for a trusted home LAN or private VPN. Do not forward its HTTP port directly through an Internet-facing router.

## API token

An optional API token can protect media catalog, administrative settings, reports, and playback-control endpoints. Use a long random token when the network is shared with untrusted clients.

The token digest is stored in `/var/lib/sylc-mvc-stream/config.json`. The clear token is not retained by the server.

HLS playlists and segments are intentionally available through their randomized session URLs because many playback clients cannot attach custom authentication headers to every HLS request. Treat those URLs as temporary bearer URLs.

## Media access

- Source libraries are read-only from the application's perspective.
- The application does not expose a general file-download endpoint.
- The directory browser returns directories only and is restricted to configured browse roots.
- Paths are canonicalized before use.
- Directory and media-file symlinks are not followed by catalog scanning.
- Removing a media library does not delete or alter files.

## Reporting vulnerabilities

For a public repository, use GitHub's private security-advisory feature rather than opening a public issue for an unpatched vulnerability.
