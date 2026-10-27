// A calculator in which every key, menu item and keyboard shortcut is a
// command with a handler of its own, and the handlers are found at compile
// time. Gust: the commands are the variants of an enum, `run` has one
// overload per variant, and a `const for` over the variants expands into
// the dispatch, the Edit menu and the shortcuts. There is no table of
// callbacks and no closure kept as an object: adding a command is adding a
// variant and its functions, which the compiler then finds everywhere.
//
// Click the keys, or press Escape to clear, Backspace to take back a digit,
// Return for =, and Z to undo the last command, which Edit > Undo does
// too. `-- --keys 12+30=` plays those keys one a frame and then stops, as
// the samples' runner does headless; the last frame is saved as
// ui_calc.png.
//
//   gust 32_ui_calc.goose

import std;
import os;
import gfx;
import ui;

// Roboto, as in 29_ui_todo, read into the program when it is compiled.
let ROBOTO: const u8[:] = embed_file("data/Roboto-Regular.ttf");

// Integers only: what is typed, the operation waiting for its second
// operand, and whether the last one divided by zero.
struct Calc {
    typed: i64 = 0,
    typing: bool = false,       // digits are being typed into `typed`
    value: i64 = 0,             // the last result
    pending: u8 = ' ',          // + - * / waiting, or ' '
    error: bool = false,
}

enum Cmd {
    Digit { d: i64 },
    Op { o: u8 },
    Equals,
    Clear,
    Back,
    Negate,
    Undo,
}

// The calculator and its history, which Undo walks back.
struct App { calc: Calc, history: Calc[>..<] }

// --- the commands ------------------------------------------------------------

fn current(c: Calc&) -> i64 { if c.typing { c.typed } else { c.value } }

fn apply(c: Calc&, b: i64) {
    if c.pending == '+' { c.value += b; }
    else if c.pending == '-' { c.value -= b; }
    else if c.pending == '*' { c.value *= b; }
    else if c.pending == '/' {
        if b == 0 { c.error = true; } else { c.value /= b; }
    } else {
        c.value = b;
    }
}

fn run(k: Cmd.Digit, app: App&) {
    if !app.calc.typing { app.calc.typed = 0; app.calc.typing = true; }
    if app.calc.typed < 100000000000 { app.calc.typed = app.calc.typed * 10 + k.d; }
}
fn run(k: Cmd.Op, app: App&) {
    if app.calc.typing { apply(app.calc, app.calc.typed); }
    app.calc.typing = false;
    app.calc.pending = k.o;
}
fn run(k: Cmd.Equals, app: App&) {
    apply(app.calc, current(app.calc));
    app.calc.typing = false;
    app.calc.pending = ' ';
}
fn run(k: Cmd.Clear, app: App&) { app.calc = Calc { .. }; }
fn run(k: Cmd.Back, app: App&) { if app.calc.typing { app.calc.typed /= 10; } }
fn run(k: Cmd.Negate, app: App&) {
    if app.calc.typing { app.calc.typed = -app.calc.typed; } else { app.calc.value = -app.calc.value; }
}
fn run(k: Cmd.Undo, app: App&) {
    if app.history.len > 0 { app.calc = app.history.pop(); }
}

// What the Edit menu shows for a command, "" for none; and its key.
fn menu(k: Cmd.Digit) -> const u8[:] { "" }
fn menu(k: Cmd.Op) -> const u8[:] { "" }
fn menu(k: Cmd.Equals) -> const u8[:] { "" }
fn menu(k: Cmd.Clear) -> const u8[:] { "Clear" }
fn menu(k: Cmd.Back) -> const u8[:] { "Take back a digit" }
fn menu(k: Cmd.Negate) -> const u8[:] { "Negate" }
fn menu(k: Cmd.Undo) -> const u8[:] { "Undo" }
fn key(k: Cmd.Digit) -> const u8[:] { "" }
fn key(k: Cmd.Op) -> const u8[:] { "" }
fn key(k: Cmd.Equals) -> const u8[:] { "Return" }
fn key(k: Cmd.Clear) -> const u8[:] { "Escape" }
fn key(k: Cmd.Back) -> const u8[:] { "Backspace" }
fn key(k: Cmd.Negate) -> const u8[:] { "N" }
fn key(k: Cmd.Undo) -> const u8[:] { "Z" }

// Every command goes through here: the state before it is kept for Undo,
// and the handler is the variant's own run, chosen at compile time.
fn dispatch(app: App&, cmd: Cmd) {
    match cmd {
        const for <N, V> in variants<Cmd> {
            V k => {
                if N != "Undo" { app.history.push(app.calc); }
                run(k, app);
            },
        },
    }
}

// The command a key of the keypad (or of --keys) stands for, and whether
// it stands for one.
fn command(ch: u8) -> Cmd, bool {
    if ch >= '0' && ch <= '9' { return Cmd.Digit { d: ch - '0' }, true; }
    if ch == '+' || ch == '-' || ch == '*' || ch == '/' { return Cmd.Op { o: ch }, true; }
    if ch == '=' { return Cmd.Equals, true; }
    if ch == 'c' { return Cmd.Clear, true; }
    if ch == 'n' { return Cmd.Negate, true; }
    if ch == 'u' { return Cmd.Undo, true; }
    return Cmd.Clear, false;
}

fn shown(c: Calc&) -> u8[>..] {
    if c.error { return str("error"); }
    return str(current(c));
}

// --- the window --------------------------------------------------------------

let KEYPAD: const u8[:][4] = ["789/", "456*", "123-", "c0=+"];

fn main() {
    var script: const u8[:] = "";
    let argv = args();
    if argv.len == 3 && argv[1] == "--keys" { script = argv[2]; }
    guard gfx::open("Gust: a calculator of commands", 360, 460, 0) else {
        write_stderr(str("gfx: ", gfx::error(), "\n"));
        return;
    }
    var ctx = ui::create_from_memory(ROBOTO, 20.0);
    guard ctx.id != 0 else {
        write_stderr(str("ui: ", ui::error(), "\n"));
        return;
    }
    var app = App { calc: Calc { .. }, history: [] };
    var played = 0;
    var drawn = 0;

    while gfx::frame() {
        // The keyboard's shortcuts, one test per command that has one.
        const for <N, V> in variants<Cmd> {
            let k = key(default<V>());
            if k.len > 0 && gfx::key_pressed(k) { dispatch(app, default<V>()); }
        }
        // A scripted key a frame, as if clicked.
        if played < script.len {
            let cmd, ok = command(script[played]);
            if ok { dispatch(app, cmd); }
            played++;
        }
        ui::input_from_gfx(ctx);

        if ui::begin(ctx, "Calculator", ui::rect(10, 10, 340, 440), ui::WINDOW_BORDER | ui::WINDOW_TITLE) {
            // The Edit menu: the commands that have a menu label, in the
            // order the enum declares them.
            ui::menubar_begin(ctx);
            ui::layout_row_static(ctx, 24.0, 60, 1);
            if ui::menu_begin_label(ctx, "Edit", ui::TEXT_LEFT, float2 { 200.0, 160.0 }) {
                ui::layout_row_dynamic(ctx, 26.0, 1);
                const for <N, V> in variants<Cmd> {
                    let m = menu(default<V>());
                    if m.len > 0 && ui::menu_item_label(ctx, m, ui::TEXT_LEFT) { dispatch(app, default<V>()); }
                }
                ui::menu_end(ctx);
            }
            ui::menubar_end(ctx);

            ui::layout_row_dynamic(ctx, 50.0, 1);
            ui::label(ctx, str(shown(app.calc), if app.calc.pending == ' ' { "" } else { "  ..." }),
                      ui::TEXT_RIGHT);
            for row in KEYPAD {
                ui::layout_row_dynamic(ctx, 56.0, 4);
                for i in row.len {
                    if ui::button_label(ctx, row[i..i + 1]) {
                        let cmd, ok = command(row[i]);
                        if ok { dispatch(app, cmd); }
                    }
                }
            }
            ui::layout_row_dynamic(ctx, 40.0, 2);
            if ui::button_label(ctx, "+/-") { dispatch(app, Cmd.Negate); }
            if ui::button_label(ctx, "Undo") { dispatch(app, Cmd.Undo); }
        }
        ui::end(ctx);

        gfx::begin_screen(float4 { 0.15, 0.17, 0.20, 1.0 });
        gfx::end_pass();
        ui::render(ctx);
        drawn++;
        if script.len > 0 && played == script.len { break; }
    }

    let saved = gfx::screenshot("ui_calc.png");
    print("drew ", drawn, " frames; shows ", shown(app.calc), "; ", app.history.len,
          " commands to undo; saved ui_calc.png: ", saved);
    ui::destroy(ctx);
    gfx::close();
}
