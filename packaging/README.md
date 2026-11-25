# Packaging & Native Bundles

`markee` includes ready-to-use packaging scripts for macOS, Homebrew, Debian/Ubuntu, and portable release tarballs.

## macOS `.app` Bundle & Login Item

Build `build/Markee.app` (requires `raylib`):

```bash
make app
# Or with Dock icon hidden (LSUIElement) and bundled libraylib:
./packaging/build-macos-app.sh --agent --standalone --version 1.0.0
```

Install into `/Applications` and optionally launch automatically at login:

```bash
cp -R build/Markee.app /Applications/
cp packaging/com.markee.display.plist ~/Library/LaunchAgents/
launchctl load -w ~/Library/LaunchAgents/com.markee.display.plist
```

Customize window defaults (zoom, case theme, corner, idle dimming, auth token) in `~/.config/markee/config.ini`.

## Homebrew Formula

[`packaging/markee.rb`](markee.rb) is a Homebrew formula with a built-in `brew services` definition:

```bash
brew install --HEAD ./packaging/markee.rb
brew services start markee
```

For tagged releases, update `url` and `sha256` in `packaging/markee.rb`.

## Debian / Ubuntu `.deb`

On Debian or Ubuntu (with `dpkg-deb` and `libraylib-dev` installed):

```bash
./packaging/build-deb.sh --version 1.0.0
sudo apt install ./build/markee_1.0.0_*.deb
systemctl --user enable --now markee
```

## Release Tarball

Build a portable `build/markee-<version>-<os>-<arch>.tar.gz`:

```bash
make tarball
```
