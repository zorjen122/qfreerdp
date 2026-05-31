import QtQuick
import QtQuick.Controls
import MyTestApp

Window {
    width: 1024
    height: 768
    visible: true
    title: qsTr("Qt6 QML FreeRDP Test in WSL2")

    RdpViewItem {
        id: rdpItem
        objectName: "rdpViewItem"
        anchors.fill: parent
        focus: true

        Component.onCompleted: forceActiveFocus()
    }
}
