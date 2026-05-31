import QtQuick
import QtQuick.Controls

Window {
    width: 640
    height: 480
    visible: true
    title: qsTr("Qt6 QML Test in WSL2")

    Rectangle {
        anchors.fill: parent
        color: "#f0f3f6"

        Column {
            anchors.centerIn: parent
            spacing: 20

            Text {
                text: "Hello, Qt 6 & QML 6!"
                font.pointSize: 24
                font.bold: true
                color: "#2c3e50"
                anchors.horizontalCenter: parent.horizontalCenter
            }

            Button {
                text: "Click Me"
                anchors.horizontalCenter: parent.horizontalCenter
                onClicked: {
                    console.log("Button in QML was clicked successfully!")
                }
            }
        }
    }
}
