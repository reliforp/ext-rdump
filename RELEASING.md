# Releasing

ext-rdump is published to two channels:

- **PIE / Packagist** for modern setups (PIE itself needs PHP 8.1+, though the
  extension builds on 7.0+).
- **PECL** (`pecl install rdump`) for older environments, where the bundled
  `pecl` tool is the natural installer.

The version lives in three places that must agree: `PHP_RDUMP_VERSION` in
`php_rdump.h`, `<release>`/`<api>` in `package.xml`, and the git tag. The
release workflow fails if they drift.

## Cutting a release

1. Bump the version in `php_rdump.h` and `package.xml` (also update
   `package.xml`'s `<date>`/`<notes>` and `CHANGELOG.md`).
2. Merge to `main`.
3. Tag and push: `git tag -a vX.Y.Z -m "ext-rdump X.Y.Z" && git push origin vX.Y.Z`.
4. The `release` workflow validates `package.xml`, builds, runs the tests,
   produces `rdump-X.Y.Z.tgz`, and attaches it to the GitHub Release.

## Packagist (PIE) — one-time

1. Submit `https://github.com/reliforp/ext-rdump` at
   <https://packagist.org/packages/submit>.
2. Enable the GitHub hook / Packagist app so new tags auto-update.

After that, each pushed `vX.Y.Z` tag is picked up automatically;
`pie install reliforp/ext-rdump` resolves it.

## PECL — one-time, then per-release

1. Register an account at <https://pecl.php.net>.
2. Propose the package on the `pecl-dev` mailing list and wait for a maintainer
   to grant karma for the `rdump` package (new packages require approval).
3. Once approved, upload `rdump-X.Y.Z.tgz` (the artifact from the release
   workflow, or `pecl package` locally) via the PECL release form.

Per release after approval: upload the new tarball. `pecl install rdump`
resolves the latest stable; while `<stability>` is `beta`, users install with
`pecl install rdump-beta`.
