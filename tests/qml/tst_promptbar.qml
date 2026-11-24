import QtQuick
import QtTest
import MightyOS.Core
import MightyOS.UI

// PromptBar glyphs: the style follows the device used last; gamepad glyphs
// follow InputRouter.gamepadStyle, positionally (Accept is the south
// button: A on Xbox, Cross on PlayStation, B on Nintendo).
Item {
    id: root
    width: 1600
    height: 200

    PromptBar {
        id: bar
        prompts: [
            { action: InputRouter.Accept, label: "Play" },
            { action: InputRouter.Back, label: "Back" },
            { action: InputRouter.Favorite, label: "Favorite" },
            { action: InputRouter.Search, label: "Search" },
            { action: InputRouter.Options, label: "Options" },
            { action: InputRouter.NextCategory, label: "Category",
              actions: [InputRouter.PrevCategory, InputRouter.NextCategory] },
            { action: InputRouter.PageDown, label: "Page",
              actions: [InputRouter.PageUp, InputRouter.PageDown] }
        ]
    }

    TestCase {
        name: "PromptBar"
        when: windowShown

        function init() {
            InputRouter.blocked = false
            InputRouter.muteFor(0)
            InputRouter.gamepadStyle = InputRouter.Xbox
        }

        function cleanup() {
            InputRouter.gamepadStyle = InputRouter.Xbox
            useSource(InputRouter.Keyboard)
        }

        function useSource(source) {
            // Any delivered action records its source.
            InputRouter.dispatch(InputRouter.Navigate, "left", source)
        }

        // Labels of all glyphs, prompt by prompt: [["A"], ["B"], ..., ["LB","RB"], ...]
        function labels() {
            const result = []
            for (let i = 0; i < bar.prompts.length; ++i) {
                const prompt = findChild(bar, "prompt" + i)
                verify(prompt, "prompt" + i)
                const glyphs = []
                collect(prompt, glyphs)
                result.push(glyphs)
            }
            return result
        }
        function collect(item, out) {
            for (const child of item.children) {
                if (child.objectName === "glyph")
                    out.push(child.label)
                else
                    collect(child, out)
            }
        }

        function test_keyboard() {
            useSource(InputRouter.Keyboard)
            compare(bar.glyphStyle, "keyboard")
            compare(labels(), [["Enter"], ["Esc"], ["Ins"], ["/"], ["Tab"], ["PgUp", "PgDn"], ["Home", "End"]])
            useSource(InputRouter.Mouse)
            compare(bar.glyphStyle, "keyboard")
        }

        function test_xbox() {
            useSource(InputRouter.Gamepad)
            compare(bar.glyphStyle, "xbox")
            compare(labels(), [["A"], ["B"], ["X"], ["Y"], ["Menu"], ["LB", "RB"], ["LT", "RT"]])
        }

        function test_playstation() {
            InputRouter.gamepadStyle = InputRouter.PlayStation
            useSource(InputRouter.Gamepad)
            compare(bar.glyphStyle, "playstation")
            compare(labels(), [["Cross"], ["Circle"], ["Square"], ["Triangle"], ["Options"],
                               ["L1", "R1"], ["L2", "R2"]])
        }

        function test_nintendoIsPositional() {
            InputRouter.gamepadStyle = InputRouter.Nintendo
            useSource(InputRouter.Gamepad)
            compare(bar.glyphStyle, "nintendo")
            // South = Accept shows B, east = Back shows A, west Y, north X.
            compare(labels(), [["B"], ["A"], ["Y"], ["X"], ["+"], ["L", "R"], ["ZL", "ZR"]])
        }

        function test_remote() {
            InputRouter.gamepadStyle = InputRouter.PlayStation   // irrelevant for the remote
            useSource(InputRouter.Remote)
            compare(bar.glyphStyle, "remote")
            // Colour keys have no text on them: their label (and accessible
            // name) is the colour.
            compare(labels(), [["OK"], ["Back"], ["Blue"], ["Yellow"], ["Red"],
                               ["Rewind", "Fast forward"], ["CH+", "CH\u2212"]])
            const blue = findChild(findChild(bar, "prompt2"), "glyph")
            compare(blue.Accessible.name, "Blue")
            compare(blue.spec.shape, "dot")
        }

        function test_styleChangesWithTheLastDevice() {
            useSource(InputRouter.Gamepad)
            compare(bar.glyphStyle, "xbox")
            InputRouter.gamepadStyle = InputRouter.Nintendo
            compare(bar.glyphStyle, "nintendo")
            useSource(InputRouter.Keyboard)
            compare(bar.glyphStyle, "keyboard")
        }

        function test_everyGlyphHasAnAccessibleName() {
            for (const source of [InputRouter.Keyboard, InputRouter.Gamepad, InputRouter.Remote]) {
                for (const style of [InputRouter.Xbox, InputRouter.PlayStation, InputRouter.Nintendo]) {
                    InputRouter.gamepadStyle = style
                    useSource(source)
                    for (let i = 0; i < bar.prompts.length; ++i) {
                        const glyphs = []
                        collectItems(findChild(bar, "prompt" + i), glyphs)
                        verify(glyphs.length > 0)
                        for (const g of glyphs)
                            verify(g.Accessible.name.length > 0, bar.glyphStyle + " prompt " + i)
                    }
                }
            }
        }
        function collectItems(item, out) {
            for (const child of item.children) {
                if (child.objectName === "glyph")
                    out.push(child)
                else
                    collectItems(child, out)
            }
        }

        function test_glyphsScaleWithTheTheme() {
            useSource(InputRouter.Gamepad)
            const glyph = findChild(findChild(bar, "prompt0"), "glyph")
            verify(glyph)
            compare(glyph.height, Theme.px(44))
            verify(glyph.width >= glyph.height)
        }
    }
}
