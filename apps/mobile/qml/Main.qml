import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtWebView

ApplicationWindow {
    id: window
    width: 420
    height: 760
    minimumWidth: 360
    minimumHeight: 640
    visible: true
    title: qsTr("NCS 充电")
    color: "#f4f7fb"

    header: ToolBar {
        height: 56
        background: Rectangle {
            color: "#1769e0"
        }
        Label {
            anchors.centerIn: parent
            text: qsTr("北京充电站")
            color: "white"
            font.pixelSize: 19
            font.bold: true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        Frame {
            Layout.fillWidth: true
            padding: 10

            RowLayout {
                anchors.fill: parent
                Label {
                    text: qsTr("北京市")
                    font.bold: true
                }
                TextField {
                    Layout.fillWidth: true
                    placeholderText: qsTr("输入北京地址")
                    enabled: false
                }
                Button {
                    text: qsTr("定位")
                    enabled: false
                }
            }
        }

        Frame {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: 0
            clip: true

            Loader {
                id: mapLoader
                anchors.fill: parent
                active: !ncsSmokeTest && mapConfiguration.configured
                sourceComponent: Component {
                    WebView {
                        id: mapView

                        Component.onCompleted: {
                            if (mapConfiguration.remotePageUrl.length > 0)
                                url = mapConfiguration.remotePageUrl;
                            else
                                loadHtml(mapConfiguration.mapHtml, mapConfiguration.javascriptOrigin);
                        }

                        onLoadingChanged: function (loadRequest) {
                            if (loadRequest.status === WebView.LoadSucceededStatus)
                                statusBanner.text = qsTr("地图已加载");
                            else if (loadRequest.status === WebView.LoadFailedStatus)
                                statusBanner.text = qsTr("地图加载失败：") + loadRequest.errorString;
                        }
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                width: parent.width - 40
                spacing: 10
                visible: !mapLoader.active

                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("地图暂不可用")
                    font.pixelSize: 20
                    font.bold: true
                }
                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: ncsSmokeTest ? qsTr("烟雾测试不会访问外部地图服务。") : mapConfiguration.statusMessage
                    color: "#64748b"
                }
            }

            Label {
                id: statusBanner
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                padding: 8
                text: mapConfiguration.statusMessage
                color: "white"
                background: Rectangle {
                    color: "#b3212937"
                }
                visible: mapLoader.active
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Repeater {
                model: [qsTr("附近"), qsTr("充电"), qsTr("订单"), qsTr("我的")]
                Button {
                    Layout.fillWidth: true
                    text: modelData
                    flat: index !== 0
                    enabled: index === 0
                }
            }
        }
    }
}
