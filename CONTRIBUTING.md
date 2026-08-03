# Contributing

Bug reports should include:

- server version;
- Linux distribution;
- CPU/GPU and VA-API device;
- source type (MVC MKV/MK3D or unencrypted Blu-ray 3D ISO);
- selected output and audio modes;
- the downloaded SyLC diagnostic report;
- whether the issue occurs from time zero, after a seek, or both.

Do not upload copyrighted movie files or decrypted commercial-disc content to the repository.

Before submitting code, run:

```bash
./tests/run_all.sh
```

Native code should preserve the existing third-party license notices and the clean-room boundary described in `docs/SOURCE_PROVENANCE.md`.
