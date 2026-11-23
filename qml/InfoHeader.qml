import QtQuick

// Big heading plus one detail line, used above rows and grids to describe
// the focused item (title, full system name, year, ...).
Column {
    id: header

    property string heading
    property string detail

    spacing: Theme.spacingS

    Text {
        textFormat: Text.PlainText
        width: header.width
        text: header.heading
        color: Theme.text
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontTitle
        font.weight: Font.Bold
        elide: Text.ElideRight
    }
    Text {
        textFormat: Text.PlainText
        width: header.width
        text: header.detail
        color: Theme.textDim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
        elide: Text.ElideRight
    }
}
