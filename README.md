# htmltext-plugin

A minimal Claws Mail plugin that renders `text/html` MIME parts as plain text.

## Architecture

```
claws-mail
  └── htmltext_viewer.so   (plugin directory – C plugin)
        │  registers a MimeViewer for text/html
        │  extracts MIME part to temp file
        │  pipes raw HTML to stdin of:
        └── htmltext-render   (render directory – Rust binary)
              reads  stdin:  raw HTML bytes
              writes stdout: plain text (UTF-8)
              no network I/O, no filesystem side-effects
```

## Security guarantees

* `htmltext-render` never makes network connections.
* CSS, images, `<script>`, `<iframe>` tags are stripped before rendering.
* The plugin passes no environment variables to the child beyond the
  inherited set; sandboxing (e.g. systemd-run, bubblewrap) can be added
  later by wrapping the exec path.

## Build

### Plugin (C)

```sh
cd plugin
# Requires: gcc, pkg-config, gtk+-3.0-dev, claws-mail headers
make
sudo make install          # installs to /usr/local/lib/claws-mail/plugins/
# or:
sudo make install PREFIX=/usr
```

### Render binary (Rust)

```sh
cd render
cargo build --release
sudo install -m 755 target/release/htmltext-render /usr/local/bin/
```

## Configuration

Load the plugin in Claws Mail: *Configuration → Plugins → Load…*
and select `htmltext_viewer.so`.

The companion binary must be named `htmltext-render` and be present in
your `$PATH`, or you can hard-code its path at compile time:

```sh
make CFLAGS_EXTRA='-DHTMLTEXT_RENDER_BIN=\"/usr/lib/claws-mail/htmltext-render\"'
```
