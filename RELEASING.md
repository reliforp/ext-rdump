# Releasing

ext-rdump is published through **PIE / Packagist**. (pecl.php.net is deprecated
and no longer accepts new packages, so there is no central PECL channel; users
on older PHP install from a checkout with `pecl install package.xml`, which just
builds locally. See the README.)

The version lives in three places that must agree: `PHP_RDUMP_VERSION` in
`php_rdump.h`, `<release>`/`<api>` in `package.xml`, and the git tag. The
release workflow fails if they drift.

## Cutting a release

1. Bump the version in `php_rdump.h` and `package.xml` (also update
   `package.xml`'s `<date>`/`<notes>` and `CHANGELOG.md`).
2. Merge to `main`.
3. Tag and push: `git tag -a vX.Y.Z -m "ext-rdump X.Y.Z" && git push origin vX.Y.Z`.
4. The `release` workflow (tag-push only) checks the version is in sync,
   validates `package.xml` and `composer.json`, builds, runs the tests,
   produces `rdump-X.Y.Z.tgz`, and attaches it to the GitHub Release as a
   convenience source archive.

The workflow runs only on a tag push (no `workflow_dispatch`), since its checks
compare against the tag. To rehearse them before tagging, run the same steps
locally:

```bash
composer validate --strict
pecl package-validate
docker run --rm -e CI=1 -v "$PWD":/ext -w /ext php:8.3-cli sh ci/build-and-test.sh
```

## Packagist (PIE): one-time

1. Submit `https://github.com/reliforp/ext-rdump` at
   <https://packagist.org/packages/submit>.
2. Enable the GitHub hook / Packagist app so new tags auto-update.

After that, each pushed `vX.Y.Z` tag is picked up automatically;
`pie install reliforp/ext-rdump` resolves it.
