use alloc::{string::String, vec::Vec};
use reflex::{display, text};

const PAPER: u16 = 0xf7bd;
const INK: u16 = 0x1947;
const TEAL: u16 = 0x034c;

fn rect(pixels: &mut [u16], x: usize, y: usize, w: usize, h: usize, color: u16) {
    for row in y..y + h {
        pixels[row * display::WIDTH + x..row * display::WIDTH + x + w].fill(color);
    }
}

pub fn draw(pixels: &mut [u16], loaded: usize, status: &str) {
    pixels.fill(PAPER);
    rect(pixels, 0, 0, 12, display::HEIGHT, TEAL);
    let label = |pixels: &mut [u16], x, y, value: &str, fg, bg, scale| {
        text::draw(
            |x, y, c| pixels[y * display::WIDTH + x] = c,
            x,
            y,
            value,
            fg,
            bg,
            scale,
        );
    };
    label(pixels, 44, 34, "Brainux", INK, PAPER, 3);
    label(pixels, 46, 120, "Linux on your Brain.", TEAL, PAPER, 1);
    label(pixels, 620, 44, "SHARP PW-H1", INK, PAPER, 1);
    label(pixels, 620, 76, "V690_H1", TEAL, PAPER, 1);
    if loaded != 0 {
        rect(pixels, 44, 184, 766, 100, TEAL);
        label(pixels, 68, 200, "Start Brainux", 0xffff, TEAL, 2);
        label(pixels, 68, 252, "SD card", 0xffff, TEAL, 1);
        label(pixels, 704, 220, "ENTER", 0xffff, TEAL, 1);
        label(pixels, 68, 312, "Diagnostic shell", INK, PAPER, 1);
        label(pixels, 752, 312, "D", TEAL, PAPER, 1);
        rect(pixels, 44, 362, 766, 1, 0xbdf7);
        // Error details remain visible after a failed launch.
        for (row, chunk) in status
            .chars()
            .collect::<Vec<_>>()
            .chunks(62)
            .take(2)
            .enumerate()
        {
            let line: String = chunk.iter().collect();
            label(pixels, 46, 374 + row * 26, &line, INK, PAPER, 1);
        }
    } else {
        label(pixels, 46, 192, "Unable to start", 0x9800, PAPER, 2);
        for (row, chunk) in status
            .chars()
            .collect::<Vec<_>>()
            .chunks(62)
            .take(4)
            .enumerate()
        {
            let line: String = chunk.iter().collect();
            label(pixels, 46, 260 + row * 26, &line, INK, PAPER, 1);
        }
    }
    label(pixels, 46, 438, "ESC  Back to dictionary", INK, PAPER, 1);
}
