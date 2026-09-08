import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: page
    property var station: ({})
    property var chargers: []
    property int selectedId: 0
    property bool busy: false
    property bool hasActiveFlow: false
    property string errorMessage: ""
    property bool expanded: false
    property var stationReviews: []
    property bool stationReviewsBusy: false
    property string stationReviewsError: ""
    readonly property color ink: "#243F30"
    readonly property color green: "#23794E"
    readonly property color muted: "#607362"
    readonly property var selectedCharger: chargers.find(c => Number(c.id) === selectedId) || ({})
    readonly property int idleCount: chargers.filter(c => Number(c.status) === 0).length
    signal ordersRequested
    signal backRequested
    signal navigationRequested
    signal reserveRequested
    signal activeFlowRequested
    signal refreshRequested
    signal chargerSelected(int chargerId, int chargerType)
    onStationChanged: {
        expanded = false;
        detailScroll.contentItem.contentY = 0;
    }
    function price(value) {
        return (Number(value || 0) / 100).toFixed(2);
    }
    function choose(charger) {
        if (busy || Number(charger.status) !== 0)
            return;
        chargerSelected(selectedId === Number(charger.id) ? 0 : Number(charger.id), Number(charger.chargerType));
    }
    component BodyText: Label {
        color: page.ink
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        font.pixelSize: 14
    }
    component Card: Rectangle {
        color: "white"
        radius: 18
        border.color: "#E5E9E7"
        Layout.fillWidth: true
    }
    component Action: Button {
        implicitHeight: 44
        font.pixelSize: 14
        background: Rectangle {
            radius: 12
            color: parent.enabled ? "#E4F0DC" : "#E9ECEA"
        }
        contentItem: Label {
            text: parent.text
            color: parent.enabled ? page.green : "#87938C"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Action {
                text: "‹ 返回"
                Layout.preferredWidth: 76
                flat: true
                onClicked: page.backRequested()
                background: Item {}
            }
            BodyText {
                text: "场站详情"
                font.pixelSize: 18
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }
            Action {
                text: "刷新"
                Layout.preferredWidth: 76
                enabled: !page.busy
                onClicked: page.refreshRequested()
            }
        }
        ScrollView {
            id: detailScroll
            objectName: "stationDetailScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                width: detailScroll.availableWidth
                spacing: 12
                Card {
                    implicitHeight: info.implicitHeight + 32
                    ColumnLayout {
                        id: info
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 8
                        BodyText {
                            text: page.station.name || "充电站"
                            font.pixelSize: 20
                            font.bold: true
                        }
                        BodyText {
                            text: "空闲 " + page.idleCount + "/" + page.chargers.length + " · 距您 " + (Number(page.station.distanceMeter || 0) / 1000).toFixed(1) + " km"
                            color: page.muted
                            font.pixelSize: 13
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            BodyText {
                                text: page.station.address || "暂无地址"
                                color: page.muted
                                font.pixelSize: 13
                            }
                            Action {
                                text: "导航"
                                Layout.preferredWidth: 64
                                onClicked: page.navigationRequested()
                            }
                        }
                    }
                }
                Card {
                    color: "#FFF7EC"
                    border.color: "#FFE0B8"
                    implicitHeight: fees.implicitHeight + 32
                    ColumnLayout {
                        id: fees
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 10
                        RowLayout {
                            Layout.fillWidth: true
                            BodyText {
                                text: "充电费"
                                font.pixelSize: 17
                                font.bold: true
                            }
                            Label {
                                text: "当前参考价"
                                color: "#9A5B13"
                                font.pixelSize: 12
                            }
                        }
                        RowLayout {
                            Label {
                                text: page.price(page.station.totalPriceCentPerKwh)
                                font.pixelSize: 34
                                font.bold: true
                                color: "#D95E00"
                            }
                            Label {
                                text: "元/度"
                                color: "#8A6A3B"
                                font.pixelSize: 13
                                Layout.alignment: Qt.AlignBottom
                                Layout.bottomMargin: 6
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Repeater {
                                model: [
                                    {
                                        label: "电费",
                                        value: page.station.electricityPriceCentPerKwh
                                    },
                                    {
                                        label: "服务费",
                                        value: page.station.servicePriceCentPerKwh
                                    }
                                ]
                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: 66
                                    radius: 10
                                    color: "#FFFBF3"
                                    border.color: "#F3DDBB"
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 10
                                        spacing: 3
                                        BodyText {
                                            text: modelData.label
                                            font.pixelSize: 12
                                            color: "#B0824A"
                                        }
                                        BodyText {
                                            text: page.price(modelData.value) + " 元/度"
                                            font.pixelSize: 14
                                            font.bold: true
                                        }
                                    }
                                }
                            }
                        }
                        BodyText {
                            text: "最终单价以预约后确认的报价为准"
                            color: "#9A7041"
                            font.pixelSize: 12
                        }
                    }
                }
                Card {
                    implicitHeight: chargerContent.implicitHeight + 32
                    ColumnLayout {
                        id: chargerContent
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 10
                        RowLayout {
                            Layout.fillWidth: true
                            BodyText {
                                text: "枪桩信息"
                                font.pixelSize: 16
                                font.bold: true
                            }
                            Label {
                                text: "空闲 " + page.idleCount + "/" + page.chargers.length
                                color: page.idleCount ? page.green : "#B42318"
                                font.pixelSize: 13
                            }
                        }
                        BodyText {
                            text: "已选：" + (page.selectedCharger.code || "")
                            visible: page.selectedId > 0
                            color: page.green
                            font.pixelSize: 12
                        }
                        BodyText {
                            text: page.busy ? "正在加载电桩…" : "该站点暂无电桩，请刷新重试"
                            visible: page.chargers.length === 0
                            color: page.muted
                        }
                        Repeater {
                            model: page.expanded ? page.chargers : page.chargers.slice(0, 3)
                            Rectangle {
                                id: chargerCard
                                readonly property bool selected: page.selectedId === Number(modelData.id)
                                readonly property bool available: Number(modelData.status) === 0
                                Layout.fillWidth: true
                                implicitHeight: Math.max(92, chargerRow.implicitHeight + 24)
                                radius: 14
                                color: selected ? "#DDF5EF" : "white"
                                border.width: selected ? 2 : 1
                                border.color: selected ? page.green : "#DDEBE8"
                                RowLayout {
                                    id: chargerRow
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 8
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 5
                                        BodyText {
                                            text: modelData.code || "充电桩"
                                            font.pixelSize: 15
                                            font.bold: true
                                            wrapMode: Text.WrapAnywhere
                                        }
                                        BodyText {
                                            text: (Number(modelData.chargerType) === 1 ? "快充" : "慢充") + " · 累计 " + (modelData.totalCount || 0) + " 次"
                                            color: page.muted
                                            font.pixelSize: 12
                                        }
                                        BodyText {
                                            text: chargerCard.selected ? "✓ 已选择" : (modelData.statusText || "未知")
                                            font.pixelSize: 12
                                            color: chargerCard.available ? page.green : Number(modelData.status) === 2 ? "#B42318" : "#886719"
                                        }
                                    }
                                    Label {
                                        text: Number(modelData.powerWatt || 0) / 1000 + "\nkW"
                                        color: page.green
                                        font.bold: true
                                        font.pixelSize: 18
                                        horizontalAlignment: Text.AlignHCenter
                                    }
                                    Action {
                                        objectName: "chargerChoice" + modelData.id
                                        text: chargerCard.available ? (chargerCard.selected ? "取消" : "选择") : (modelData.statusText || "不可用")
                                        Layout.preferredWidth: 60
                                        enabled: chargerCard.available && !page.busy
                                        onClicked: page.choose(modelData)
                                    }
                                }
                            }
                        }
                        Action {
                            objectName: "expandChargers"
                            text: page.expanded ? "收起电桩" : "展开更多 " + Math.max(0, page.chargers.length - 3) + " 个桩"
                            visible: page.chargers.length > 3
                            Layout.fillWidth: true
                            onClicked: page.expanded = !page.expanded
                        }
                    }
                }
                Card {
                    implicitHeight: reviews.implicitHeight + 32
                    ColumnLayout {
                        id: reviews
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 10
                        BodyText {
                            text: "车友评论"
                            font.pixelSize: 16
                            font.bold: true
                        }
                        // UC-U-12 场站评论墙：加载、失败与空态，列表由服务端按时间倒序返回。
                        BodyText {
                            visible: page.stationReviewsBusy
                            text: "正在加载评论…"
                            color: page.muted
                            font.pixelSize: 13
                        }
                        BodyText {
                            visible: !page.stationReviewsBusy && page.stationReviewsError !== ""
                            text: page.stationReviewsError
                            color: "#B42318"
                            font.pixelSize: 13
                        }
                        Repeater {
                            model: page.stationReviews
                            delegate: ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Label {
                                        text: modelData.author
                                        color: page.ink
                                        font.pixelSize: 13
                                        font.bold: true
                                        elide: Text.ElideRight
                                        Layout.maximumWidth: 120
                                    }
                                    Label {
                                        text: "★".repeat(Number(modelData.rating)) +
                                              "☆".repeat(Math.max(0, 5 - Number(modelData.rating)))
                                        color: "#F59E0B"
                                        font.pixelSize: 12
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        text: modelData.timeLabel
                                        color: "#98A2B3"
                                        font.pixelSize: 11
                                    }
                                }
                                Label {
                                    text: modelData.content
                                    color: "#475467"
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                    font.pixelSize: 13
                                }
                            }
                        }
                        BodyText {
                            visible: !page.stationReviewsBusy && page.stationReviewsError === "" &&
                                     page.stationReviews.length === 0
                            text: "暂无公开评论 · 完成充电后可在「我的订单」中评价"
                            color: page.muted
                            font.pixelSize: 13
                        }
                        Action {
                            text: "查看我的订单"
                            Layout.fillWidth: true
                            onClicked: page.ordersRequested()
                        }
                    }
                }
                BodyText {
                    visible: !!page.errorMessage
                    text: page.errorMessage
                    color: "#B42318"
                }
                Item {
                    Layout.preferredHeight: 4
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Action {
                text: "一键导航"
                Layout.fillWidth: true
                onClicked: page.navigationRequested()
            }
            Action {
                objectName: "reserveButton"
                text: page.hasActiveFlow ? "继续当前充电" : page.busy ? "处理中…" : page.selectedId ? "预约所选电桩" : "请选择空闲电桩"
                Layout.fillWidth: true
                Layout.preferredWidth: 180
                enabled: !page.busy && (page.hasActiveFlow || (page.selectedId > 0 && Number(page.selectedCharger.status) === 0))
                onClicked: page.hasActiveFlow ? page.activeFlowRequested() : page.reserveRequested()
                background: Rectangle {
                    radius: 12
                    color: parent.enabled ? "#102F46" : "#E9ECEA"
                }
                contentItem: Label {
                    text: parent.text
                    color: parent.enabled ? "white" : "#87938C"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 14
                }
            }
        }
    }
}
