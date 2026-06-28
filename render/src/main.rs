//! htmltext-render
//!
//! Reads raw HTML from stdin, writes plain text to stdout.
//! Designed to be spawned as a subprocess by the Claws Mail htmltext plugin.
//!
//! Contract with the parent process:
//!   stdin  – raw HTML bytes (UTF-8 or latin-1; html5ever handles both)
//!   stdout – plain text, UTF-8, LF line endings
//!   stderr – diagnostic messages only (human-readable, goes to terminal)
//!   exit 0 – success
//!   exit 1 – unrecoverable error (parent will show a fallback message)
//!
//! Security properties:
//!   * No network I/O whatsoever (html2text performs no resource fetching).
//!   * No filesystem writes.
//!   * All processing is in-memory on the bytes received from stdin.

use std::io::{self, Read, Write};
use std::process;

// Render width in columns. 90 is a safe default; the plugin could pass this
// as a CLI argument in a future version if desired.
const RENDER_WIDTH: usize = 90;

// Hard cap on stdin bytes we will process (same as the C plugin's cap).
const MAX_INPUT_BYTES: usize = 4 * 1024 * 1024;

fn main() {
    if let Err(e) = run() {
        eprintln!("htmltext-render: {e}");
        process::exit(1);
    }
}

// Inline rendering helper so tests don't need I/O.
fn render(html: &str) -> Result<String, Box<dyn std::error::Error>> {
    // html2text::config::plain() strips all CSS, images, scripts and links.
    // It uses html5ever (Servo's parser) internally: no network, no plugins.
    Ok(html2text::config::plain()
        .string_from_read(html.as_bytes(), RENDER_WIDTH)
        .map_err(|e| format!("html2text rendering failed: {e}"))?)
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    // --- 1. Read stdin ---------------------------------------------------
    let mut html = String::with_capacity(MAX_INPUT_BYTES);
    io::stdin()
        .take(MAX_INPUT_BYTES as u64)
        .read_to_string(&mut html)?;

    if html.is_empty() {
        // Nothing to render; exit cleanly with empty output.
        return Ok(());
    }

    // --- 2. Render HTML → plain text -------------------------------------
    let text = render(&html)?;

    // --- 3. Write to stdout ----------------------------------------------
    let stdout = io::stdout();
    let mut out = io::BufWriter::new(stdout.lock());
    out.write_all(text.as_bytes())?;
    // Ensure a trailing newline so the last line is always visible.
    if !text.ends_with('\n') {
        out.write_all(b"\n")?;
    }
    out.flush()?;

    Ok(())
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::render;

    #[test]
    fn plain_paragraph() {
        let out = render("<p>Hello, world!</p>").unwrap();
        assert!(out.contains("Hello, world!"), "got: {out:?}");
    }

    #[test]
    fn strips_script_tags() {
        let out = render("<p>visible</p><script>alert('xss')</script>").unwrap();
        assert!(out.contains("visible"), "got: {out:?}");
        assert!(!out.contains("alert"), "script content leaked: {out:?}");
    }

    #[test]
    fn strips_style_tags() {
        let out = render("<style>body { color: red }</style><p>text</p>").unwrap();
        assert!(!out.contains("color"), "style content leaked: {out:?}");
        assert!(out.contains("text"), "got: {out:?}");
    }

    #[test]
    fn strips_img_tags() {
        // No src= value should appear in the output.
        let out =
            render(r#"<img src="https://tracker.example/pixel.gif" alt=""><p>body</p>"#).unwrap();
        assert!(!out.contains("tracker.example"), "img src leaked: {out:?}");
    }

    #[test]
    fn heading_rendered() {
        let out = render("<h1>Subject</h1><p>Content here.</p>").unwrap();
        // html2text renders headings with # or ALL-CAPS depending on config;
        // at minimum the text must be present.
        assert!(out.contains("Subject"), "got: {out:?}");
        assert!(out.contains("Content here."), "got: {out:?}");
    }

    #[test]
    fn unordered_list() {
        let out = render("<ul><li>Alpha</li><li>Beta</li></ul>").unwrap();
        assert!(out.contains("Alpha"), "got: {out:?}");
        assert!(out.contains("Beta"), "got: {out:?}");
    }

    #[test]
    fn table_readable() {
        let out = render(
            "<table><tr><th>Name</th><th>Value</th></tr>\
             <tr><td>foo</td><td>bar</td></tr></table>",
        )
        .unwrap();
        assert!(out.contains("Name"), "got: {out:?}");
        assert!(out.contains("foo"), "got: {out:?}");
    }

    #[test]
    fn link_href_not_fetched() {
        // The URL must appear as annotation text at most, never be fetched.
        // We simply verify the visible text is preserved and no panic occurs.
        let out = render(r#"<a href="https://example.com">click here</a>"#).unwrap();
        assert!(out.contains("click here"), "got: {out:?}");
    }

    #[test]
    fn empty_input_produces_empty_output() {
        let out = render("").unwrap();
        // html2text on empty input returns an empty or whitespace-only string.
        assert!(out.trim().is_empty(), "got: {out:?}");
    }

    #[test]
    fn malformed_html_does_not_panic() {
        // html5ever is resilient to broken markup.
        let out = render("<p>unclosed <b>bold <i>italic</p>").unwrap();
        assert_eq!(out, "unclosed **bold italic**\n");
    }

    #[test]
    fn iso_8859_declared_but_utf8_content() {
        // Common mismatch in old mails; must not panic.
        let html = r#"<html><head><meta charset="iso-8859-1"></head>
                      <body><p>café</p></body></html>"#;
        let out = render(html).unwrap();
        assert!(out.contains("café"), "got: {out:?}"); // 'é' may vary
    }
}
