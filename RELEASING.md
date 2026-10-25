# Releasing

Releases go out from GitHub, not from a laptop. Publishing a GitHub release runs
[`.github/workflows/release.yml`](.github/workflows/release.yml):

1. it checks that every file says the release's version;
2. builds and tests the command line on Linux (x64, arm64), Windows (x64) and macOS (arm64, x64),
   through [`.github/workflows/build.yml`](.github/workflows/build.yml) and
   [`scripts/release-build.mjs`](scripts/release-build.mjs);
3. makes the npm packages and installs them as a user would, running the command line and the
   Node API;
4. attaches the five archives and `SHA256SUMS` to the release;
5. puts the npm packages out through **trusted publishing**: no npm token anywhere, and each
   version gets a provenance badge linking it to the commit. The five platform packages are
   published; `fynepdf-pdfa`, the one people install, is staged.

A maintainer then approves `fynepdf-pdfa` on npmjs.com, and that version goes live. The platform
packages do nothing until it points at them, so they need no approval of their own.

## What is published

| Where | What |
| --- | --- |
| GitHub release | `fynepdf-pdfa-<version>-linux-x64.tar.gz`, `-linux-arm64.tar.gz`, `-windows-x64.zip`, `-macos-arm64.tar.gz`, `-macos-x64.tar.gz`, and `SHA256SUMS`. Each holds the program, PDFium, the fonts and every licence. |
| npm | `fynepdf-pdfa` (the Node API and `npx fynepdf-pdfa`), and one package per platform that it installs as an optional dependency: `fynepdf-pdfa-linux-x64`, `-linux-arm64`, `-win32-x64`, `-darwin-arm64`, `-darwin-x64`. |
| The tag | `FetchContent` and vcpkg users build from it (`GIT_TAG v<version>`). |

The Linux programs are built in manylinux_2_28, so they run on glibc 2.28 and newer (Debian 10,
Ubuntu 18.10, RHEL 8 and newer); the macOS ones on macOS 11 and newer. They aren't signed with an
Apple or Microsoft certificate. A macOS user who downloads an archive in a browser clears the
quarantine with `xattr -d com.apple.quarantine fynepdf-pdfa libpdfium.dylib`.

## Each release

1. **Set the version** in a pull request and merge it once CI is green:

   ```bash
   node scripts/set-version.mjs 0.2.0
   ```

   It changes `CMakeLists.txt`, `vcpkg.json`, `npm/fynepdf-pdfa/package.json` (with the
   platform packages it depends on) and the `GIT_TAG` lines in the README and examples. Use a
   patch version (0.1.1) for fixes, a minor version (0.2.0) for new features or output changes.

2. **Publish a GitHub release.** Releases → Draft a new release:
   - Tag: `v0.2.0` (the same version, with a `v`), "Create new tag on publish", target `main`
   - Title: `v0.2.0`
   - Click **Generate release notes**, add a short summary on top
   - Publish

3. **Wait for the Release workflow** (Actions tab; about 10 minutes, longer when the vcpkg cache is
   cold). It stops before anything is published if a file disagrees with the tag or a test fails.

4. **Approve it on npm.** npmjs.com → your avatar → Staged Packages → `fynepdf-pdfa` → Approve,
   with your two-factor login. Then check:

   ```bash
   npm view fynepdf-pdfa version
   npx -y fynepdf-pdfa --version
   ```

If you reject a staged version, delete the GitHub release and its tag, fix, and release again with
the next patch version. npm never reuses a version number.

To test the whole pipeline without publishing, run the workflow by hand (Actions → Release → Run
workflow). It builds, tests and packs everything, lists what each npm package would hold, and keeps
the archives and packages as the `release` artifact.

## One-time setup

### The repository

Make it public before the first run: GitHub's runners are free for public repositories, including
the macOS, Windows and arm64 ones this needs. CI (`ci.yml`) runs the same builds on every pull
request, plus the npm packages on Linux, Windows and macOS and a build against apt's and
Homebrew's libraries.

### The npm packages

Trusted publishing is set per package, and a package has to exist before it can be set, so the
first version goes up by hand. npm needs two-factor login on your account.

1. **Merge `release.yml` into `main`**, then run the Release workflow by hand (the dry run above).
2. **Download its `release` artifact** and unzip it. `npm/out/` holds the six packages as `.tgz`
   files.
3. **Publish them from your computer**, platforms first. `npm login`, then in that folder:

   ```bash
   for file in fynepdf-pdfa-*-0.1.0.tgz fynepdf-pdfa-0.1.0.tgz; do npm publish "$file" --access public; done
   ```

   A brand-new package is processed for a few minutes: `npm view <name> versions` shows only a
   `0.0.0-stage` placeholder until the real version appears.
4. **Add the trusted publisher to each package** from the command line, which says plainly whether
   it worked. `npm trust` needs npm 11, so run it through `npx`, signed in with `npm login`:

   ```bash
   # The platform packages: CI publishes them.
   for name in fynepdf-pdfa-linux-x64 fynepdf-pdfa-linux-arm64 fynepdf-pdfa-win32-x64 fynepdf-pdfa-darwin-arm64 fynepdf-pdfa-darwin-x64; do
     npx -y npm@11 trust github "$name" --file release.yml --repository birazzzz/fynepdf-pdfa --allow-publish
   done
   # fynepdf-pdfa: CI only stages it, and you approve each version.
   npx -y npm@11 trust github fynepdf-pdfa --file release.yml --repository birazzzz/fynepdf-pdfa --allow-stage-publish
   ```

   Each asks for your two-factor login and prints `Trust configuration created successfully`.
   Check with `npx -y npm@11 trust list <name>`: it should show the repository and `release.yml`,
   with `permissions: publish` for a platform package and `permissions: stage publish` for
   `fynepdf-pdfa`.
5. **Publish the v0.1.0 GitHub release.** The workflow attaches the archives and sees that 0.1.0 is
   already on npm, so it publishes nothing. Every release after that goes through the workflow.
6. **After the first staged release works**, lock each package to it: npmjs.com → the package →
   Settings → Publishing access → "Require two-factor authentication and disallow bypass 2fa
   tokens". Trusted publishing keeps working; tokens stop.

## Updating what the build uses

- **vcpkg:** `builtin-baseline` in `vcpkg.json` is a vcpkg commit; a newer one updates qpdf,
  libjpeg-turbo, OpenJPEG and Little CMS together. CI shows whether everything still builds.
- **PDFium:** the asset names and SHA-256s in `scripts/release-build.mjs` (and
  `scripts/build-linux.sh`), from a [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries)
  release; its API page lists each asset's `sha256` digest.
- **manylinux:** the image tag and digest in `scripts/release-build.mjs`.
