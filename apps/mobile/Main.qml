import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import QtQuick.Dialogs
import QtWebView

ApplicationWindow {
    id: window
    visible: true
    width: 420
    height: 760
    minimumWidth: 360
    minimumHeight: 640
    title: qsTr("NCS 充电")
    color: "#F5F6F8"
    readonly property color ink: "#243F30"
    readonly property color muted: "#607362"
    readonly property color green: "#23794E"
    readonly property color paleGreen: "#E4F0DC"
    readonly property color border: "#E5E9E7"
    property string page: mobileApi.loggedIn ? "home" : "login"
    property var selectedStation: ({})
    property int selectedChargerId: 0
    property int selectedChargerType: 1
    property int codeCountdown: 0
    font.family: "sans-serif"
    font.pixelSize: 14
    component Field: TextField {
        color: window.ink
        implicitHeight: 44
        background: Rectangle {
            radius: 12
            color: "white"
            border.width: parent.activeFocus ? 2 : 1
            border.color: parent.activeFocus ? window.green : window.border
        }
    }
    component Sheet: Dialog {
        id: sheet
        background: Rectangle {
            radius: 16
            color: "#F5F6F8"
            border.color: window.border
        }
        footer: ActionButton {
            text: "关闭"
            onClicked: sheet.close()
        }
    }
    component ActionButton: Button {
        implicitHeight: 44
        background: Rectangle {
            radius: 12
            color: parent.enabled ? window.paleGreen : "#E9ECEA"
        }
        contentItem: Label {
            text: parent.text
            color: parent.enabled ? window.green : window.muted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    onClosing: close => {
        if (window.page !== "home" && window.page !== "login") {
            close.accepted = false;
            window.go("home");
        }
    }
    function money(cents) {
        return "¥" + (Number(cents || 0) / 100).toFixed(2);
    }
    function distance(meters) {
        return Number(meters || 0) < 1000 ? Number(meters || 0).toFixed(0) + " m" : (Number(meters || 0) / 1000).toFixed(1) + " km";
    }
    function go(target) {
        if (target === "charge")
            mobileApi.loadActiveFlow();
        Qt.inputMethod.hide();
        page = target;
        if (target === "home")
            mobileApi.loadStations(searchField.text);
        else if (target === "orders")
            mobileApi.loadOrders();
        else if (target === "profile")
            mobileApi.loadProfile();
    }
    function openStation(station) {
        selectedStation = station;
        selectedChargerId = 0;
        selectedChargerType = 1;
        page = "detail";
        mobileApi.loadStationChargers(Number(station.id));
        mobileApi.loadStationReviews(Number(station.id));
    }
    function reserve() {
        if (!selectedStation.id || selectedChargerId <= 0)
            return;
        mobileApi.requestCharge(Number(selectedStation.id), selectedChargerType, selectedChargerId);
        page = "charge";
    }
    function openReview(orderNo) {
        mobileApi.beginReview(orderNo);
        reviewDialog.open();
    }
    Connections {
        target: mobileApi
        function onChargersChanged() {
            if (!mobileApi.chargers.some(c => Number(c.id) === window.selectedChargerId && Number(c.status) === 0)) window.selectedChargerId = 0
        }
        function onCodeSent(seconds) {
            window.codeCountdown = seconds;
        }
        function onReviewChanged() {
            if (mobileApi.reviewInfo.existing) {
                reviewDialog.rating = mobileApi.reviewInfo.rating || 0;
                reviewContent.text = mobileApi.reviewInfo.content || "";
            }
        }
        function onSessionChanged() {
            Qt.inputMethod.hide();
            window.page = mobileApi.loggedIn ? "home" : "login";
            if (mobileApi.loggedIn)
                mobileApi.loadActiveFlow();
        }
        function onReceiptChanged() {
            if (Object.keys(mobileApi.receipt || {}).length > 0)
                window.page = "receipt";
        }
    }
    Timer {
        id: codeTimer
        interval: 1000
        repeat: true
        running: window.codeCountdown > 0
        onTriggered: --window.codeCountdown
    }
    Timer {
        interval: 1000
        repeat: true
        running: mobileApi.loggedIn && !!mobileApi.flow.flowNo && Qt.application.state === Qt.ApplicationActive
        onTriggered: mobileApi.loadFlowProgress()
    }
    Timer {
        id: searchTimer
        interval: 350
        onTriggered: if (mobileApi.loggedIn && window.page === "home")
            mobileApi.loadStations(searchField.text)
    }

    StackLayout {
        anchors.fill: parent
        anchors.bottomMargin: window.page === "login" ? 0 : 70
        currentIndex: window.page === "login" ? 0 : window.page === "home" ? 1 : window.page === "detail" ? 2 : window.page === "orders" ? 3 : window.page === "profile" ? 4 : window.page === "charge" ? 5 : window.page === "receipt" ? 6 : 7

        Item {
            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width - 40, 360)
                spacing: 14
                Label {
                    text: "NCS 充电"
                    color: ink
                    font.pixelSize: 30
                    font.bold: true
                    Layout.alignment: Qt.AlignHCenter
                }
                Label {
                    text: "方便每一次出发"
                    color: muted
                    font.pixelSize: 16
                    Layout.alignment: Qt.AlignHCenter
                }
                Item {
                    Layout.preferredHeight: 8
                }
                Field {
                    id: phoneField
                    Layout.fillWidth: true
                    placeholderText: "手机号"
                    maximumLength: 11
                    validator: RegularExpressionValidator {
                        regularExpression: /^1[0-9]{10}$/
                    }
                    inputMethodHints: Qt.ImhDialableCharactersOnly
                    background: Rectangle {
                        radius: 12
                        color: "#FFFFFF"
                        border.width: parent.activeFocus ? 2 : 1
                        border.color: parent.activeFocus ? green : window.border
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Field {
                        id: codeField
                        Layout.fillWidth: true
                        placeholderText: "验证码"
                        maximumLength: 6
                        validator: RegularExpressionValidator {
                            regularExpression: /^[0-9]{6}$/
                        }
                        inputMethodHints: Qt.ImhDigitsOnly
                        background: Rectangle {
                            radius: 12
                            color: "#FFFFFF"
                            border.width: parent.activeFocus ? 2 : 1
                            border.color: parent.activeFocus ? green : window.border
                        }
                    }
                    Button {
                        text: window.codeCountdown > 0 ? window.codeCountdown + " 秒" : "获取验证码"
                        enabled: !mobileApi.busy && window.codeCountdown === 0 && phoneField.acceptableInput
                        onClicked: mobileApi.requestCode(phoneField.text.trim())
                        background: Rectangle {
                            radius: 12
                            color: parent.enabled ? paleGreen : "#E9ECEA"
                        }
                        contentItem: Label {
                            text: parent.text
                            color: parent.enabled ? green : "#9AA49D"
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
                Button {
                    text: "登录"
                    enabled: !mobileApi.busy && phoneField.acceptableInput && codeField.acceptableInput
                    Layout.fillWidth: true
                    implicitHeight: 48
                    onClicked: mobileApi.login(phoneField.text.trim(), codeField.text.trim())
                    background: Rectangle {
                        radius: 14
                        color: parent.enabled ? green : "#C8D7CD"
                    }
                    contentItem: Label {
                        text: parent.text
                        color: "white"
                        font.pixelSize: 16
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                ActionButton {
                    text: "服务器设置"
                    Layout.fillWidth: true
                    onClicked: serverDialog.open()
                }
                Label {
                    text: mobileApi.message
                    color: mobileApi.messageError ? "#B42318" : green
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    Layout.fillWidth: true
                    visible: text.length > 0
                }
            }
        }

        Item {
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: "附近"
                        color: "#151A21"
                        font.pixelSize: 28
                        font.bold: true
                    }
                    Label {
                        text: "充电站"
                        color: "#7B828A"
                        font.pixelSize: 17
                        Layout.alignment: Qt.AlignBottom
                        Layout.bottomMargin: 3
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Label {
                        text: "NCS 充电"
                        color: green
                        font.pixelSize: 13
                        font.bold: true
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Field {
                        id: searchField
                        Layout.fillWidth: true
                        placeholderText: "搜索附近场站或地址"
                        onTextChanged: searchTimer.restart()
                        background: Rectangle {
                            radius: 12
                            color: "#FFFFFF"
                            border.width: parent.activeFocus ? 2 : 1
                            border.color: parent.activeFocus ? green : window.border
                        }
                    }
                    Button {
                        text: "刷新"
                        onClicked: mobileApi.loadStations(searchField.text)
                        background: Rectangle {
                            radius: 12
                            color: paleGreen
                        }
                        contentItem: Label {
                            text: parent.text
                            color: green
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        id: regionBox
                        model: ["北京中心", "中关村", "北京南站", "石景山", "通州"]
                        Layout.fillWidth: true
                        background: Rectangle {
                            radius: 12
                            color: "white"
                            border.color: window.border
                        }
                    }
                    ActionButton {
                        text: "模拟定位"
                        enabled: !mobileApi.busy
                        onClicked: mobileApi.setLocation(regionBox.currentIndex, "")
                    }
                }
                ActionButton {
                    text: mobileApi.locating ? "正在定位…" : "使用当前位置"
                    Layout.fillWidth: true
                    enabled: !mobileApi.locating
                    onClicked: mobileApi.locateDevice()
                }
                Label {
                    text: mobileApi.locationLabel
                    color: muted
                    font.pixelSize: 12
                }
                ActionButton {
                    visible: !!mobileApi.flow.flowNo
                    text: "继续当前充电 · " + (mobileApi.flow.statusText || "待处理")
                    Layout.fillWidth: true
                    onClicked: window.go("charge")
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 78
                    radius: 14
                    color: "#E8F8EF"
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        Label {
                            text: "附近可用电桩"
                            color: muted
                            Layout.fillWidth: true
                        }
                        Label {
                            text: mobileApi.stations.length + " 个站点"
                            color: green
                            font.pixelSize: 21
                            font.bold: true
                        }
                    }
                }
                Label {
                    text: mobileApi.message.length > 0 ? mobileApi.message : "选择站点查看可用电桩"
                    color: mobileApi.messageError ? "#B42318" : (mobileApi.message.length > 0 ? green : muted)
                    font.pixelSize: 12
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                ListView {
                    id: stationList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 10
                    clip: true
                    model: mobileApi.stations
                    delegate: Rectangle {
                        width: stationList.width
                        height: 154
                        radius: 14
                        color: "#FFFFFF"
                        border.width: 1
                        border.color: window.border
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 15
                            spacing: 7
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: modelData.name || "未命名充电站"
                                    color: "#1B2028"
                                    font.pixelSize: 18
                                    font.bold: true
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: window.distance(modelData.distanceMeter)
                                    color: "#555D67"
                                    font.pixelSize: 12
                                }
                            }
                            Label {
                                text: modelData.address || "地址暂无"
                                color: "#717A86"
                                font.pixelSize: 13
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: "<font size='4'>¥</font><font size='6'><b>" + (Number(modelData.totalPriceCentPerKwh || 0) / 100).toFixed(2) + "</b></font><font size='3'> / 度</font>"
                                    textFormat: Text.RichText
                                    color: "#E76B13"
                                }
                                Label {
                                    text: "空闲 " + (modelData.idleCount || 0) + " / " + (modelData.totalCount || 0)
                                    color: "#12754E"
                                    font.bold: true
                                    leftPadding: 8
                                    rightPadding: 8
                                    topPadding: 5
                                    bottomPadding: 5
                                    background: Rectangle {
                                        radius: 6
                                        color: "#EAF6EE"
                                    }
                                }
                                Item {
                                    Layout.fillWidth: true
                                }
                                Button {
                                    text: "查看详情"
                                    implicitHeight: 32
                                    onClicked: window.openStation(modelData)
                                    background: Rectangle {
                                        radius: 10
                                        color: paleGreen
                                    }
                                    contentItem: Label {
                                        text: parent.text
                                        color: green
                                        font.bold: true
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }
                            }
                        }
                    }
                    footer: Item {
                        width: stationList.width
                        height: mobileApi.stations.length === 0 ? 100 : 20
                        Label {
                            anchors.centerIn: parent
                            text: mobileApi.stations.length === 0 ? (mobileApi.busy ? "正在加载站点…" : "暂无匹配的充电站") : ""
                            color: muted
                        }
                    }
                }
            }
        }

        StationDetail {
            station: window.selectedStation
            chargers: mobileApi.chargers
            selectedId: window.selectedChargerId
            busy: mobileApi.busy
            hasActiveFlow: !!mobileApi.flow.flowNo
            errorMessage: mobileApi.messageError ? mobileApi.message : ""
            stationReviews: mobileApi.stationReviews
            stationReviewsBusy: mobileApi.stationReviewsBusy
            stationReviewsError: mobileApi.stationReviewsError
            onOrdersRequested: window.go("orders")
            onBackRequested: window.go("home")
            onNavigationRequested: {
                mobileApi.loadRoute(Number(window.selectedStation.id), "driving")
                window.page = "navigation"
            }
            onChargerSelected: (chargerId, chargerType) => {
                window.selectedChargerId = chargerId
                window.selectedChargerType = chargerType
            }
            onReserveRequested: window.reserve()
            onActiveFlowRequested: window.go("charge")
            onRefreshRequested: {
                mobileApi.loadStationChargers(Number(window.selectedStation.id))
                mobileApi.loadStationReviews(Number(window.selectedStation.id))
            }
        }

        Item {
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: "我的订单"
                        color: ink
                        font.pixelSize: 23
                        font.bold: true
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Button {
                        text: "刷新"
                        onClicked: mobileApi.loadOrders()
                        background: Rectangle {
                            radius: 9
                            color: paleGreen
                        }
                        contentItem: Label {
                            text: parent.text
                            color: green
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
                ListView {
                    id: orderList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 10
                    clip: true
                    model: mobileApi.orders
                    delegate: Rectangle {
                        width: orderList.width
                        height: 156
                        radius: 14
                        color: "#FFFFFF"
                        border.width: 1
                        border.color: window.border
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 15
                            spacing: 7
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: modelData.stationName || "充电订单"
                                    color: ink
                                    font.bold: true
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: modelData.statusText || "未知"
                                    color: green
                                    background: Rectangle {
                                        radius: 8
                                        color: "#F3F8F7"
                                    }
                                    leftPadding: 7
                                    rightPadding: 7
                                    topPadding: 4
                                    bottomPadding: 4
                                }
                            }
                            Label {
                                text: (modelData.chargerCode || "待分配") + " · " + (modelData.startedAt ? "已开始" : "等待开始")
                                color: muted
                                font.pixelSize: 12
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: "电量 " + (Number(modelData.energyMwh || 0) / 1000000).toFixed(3) + " kWh"
                                    color: "#475467"
                                    font.pixelSize: 13
                                }
                                Item {
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: window.money(modelData.amountCent)
                                    color: green
                                    font.pixelSize: 16
                                    font.bold: true
                                }
                                ActionButton {
                                    text: "详情"
                                    onClicked: mobileApi.loadOrder(modelData.orderNo)
                                }
                                Button {
                                    visible: Number(modelData.status) === 60 || modelData.statusText === "已完成"
                                    text: "评价"
                                    onClicked: window.openReview(modelData.orderNo)
                                    background: Rectangle {
                                        radius: 9
                                        color: paleGreen
                                    }
                                    contentItem: Label {
                                        text: parent.text
                                        color: green
                                        horizontalAlignment: Text.AlignHCenter
                                    }
                                }
                            }
                        }
                    }
                    footer: Item {
                        width: orderList.width
                        height: mobileApi.orders.length === 0 ? 110 : 20
                        Label {
                            anchors.centerIn: parent
                            text: mobileApi.orders.length === 0 ? (mobileApi.busy ? "正在加载订单…" : "还没有充电足迹") : ""
                            color: muted
                        }
                    }
                }
            }
        }

        ScrollView {
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: 12
                Label {
                    text: "我的账户"
                    color: ink
                    font.pixelSize: 23
                    font.bold: true
                    Layout.margins: 16
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    implicitHeight: profileForm.implicitHeight + 32
                    radius: 14
                    color: "white"
                    border.color: window.border
                    ColumnLayout {
                        id: profileForm
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12
                        RowLayout {
                            Layout.fillWidth: true
                            Rectangle {
                                width: 68
                                height: 68
                                radius: 34
                                color: "#E5E9E7"
                                clip: true
                                Canvas {
                                    anchors.fill: parent
                                    property string avatarSource: mobileApi.avatarData ? "data:image/jpeg;base64," + mobileApi.avatarData : ""
                                    onAvatarSourceChanged: {
                                        if (avatarSource)
                                            loadImage(avatarSource);
                                        requestPaint();
                                    }
                                    onImageLoaded: requestPaint()
                                    onPaint: {
                                        const ctx = getContext("2d");
                                        ctx.clearRect(0, 0, width, height);
                                        if (!avatarSource || !isImageLoaded(avatarSource))
                                            return;
                                        ctx.save();
                                        ctx.beginPath();
                                        ctx.arc(width / 2, height / 2, width / 2, 0, 2 * Math.PI);
                                        ctx.clip();
                                        ctx.drawImage(avatarSource, 0, 0, width, height);
                                        ctx.restore();
                                    }
                                }
                                Label {
                                    anchors.centerIn: parent
                                    text: mobileApi.avatarData ? "" : "NCS"
                                    color: green
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: avatarChoice.open()
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: mobileApi.nickname || "未设置昵称"
                                    color: ink
                                    font.bold: true
                                }
                                Label {
                                    text: mobileApi.phone
                                    color: muted
                                }
                                Label {
                                    text: "注册于 " + mobileApi.createdAt
                                    color: muted
                                    font.pixelSize: 12
                                }
                            }
                        }
                        Field {
                            id: nicknameEdit
                            text: mobileApi.nickname
                            placeholderText: "昵称（1～20 字）"
                            maximumLength: 20
                            Layout.fillWidth: true
                        }
                        Label {
                            text: "余额  " + window.money(mobileApi.balanceCent)
                            color: green
                            font.pixelSize: 24
                            font.bold: true
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            ActionButton {
                                text: mobileApi.avatarUploading ? "上传中…" : "更换头像"
                                enabled: !mobileApi.busy
                                Layout.fillWidth: true
                                onClicked: avatarChoice.open()
                            }
                            ActionButton {
                                text: "保存昵称"
                                enabled: !mobileApi.busy
                                Layout.fillWidth: true
                                onClicked: mobileApi.updateNickname(nicknameEdit.text)
                            }
                        }
                    }
                }
                ActionButton {
                    text: "余额充值"
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    onClicked: rechargeDialog.open()
                }
                ActionButton {
                    text: "退出登录"
                    enabled: !mobileApi.busy
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    onClicked: mobileApi.logout()
                }
                ActionButton {
                    text: "申请注销账户"
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    onClicked: deletionDialog.open()
                }
                Label {
                    text: mobileApi.message
                    color: mobileApi.messageError ? "#B42318" : green
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.margins: 16
                }
            }
        }

        Item {
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        text: "‹ 返回"
                        onClicked: window.go("home")
                        background: Rectangle {
                            color: "transparent"
                        }
                        contentItem: Label {
                            text: parent.text
                            color: green
                        }
                    }
                    Label {
                        text: "充电控制"
                        color: ink
                        font.pixelSize: 23
                        font.bold: true
                        Layout.fillWidth: true
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 86
                    radius: 12
                    color: "#E8F8EF"
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        Label {
                            text: mobileApi.flow.statusText || "正在提交请求…"
                            color: green
                            font.pixelSize: 17
                            font.bold: true
                        }
                        Label {
                            text: mobileApi.flow.flowNo ? "流程 " + mobileApi.flow.flowNo : "请稍候，正在分配充电桩"
                            color: muted
                            font.pixelSize: 12
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 124
                    radius: 14
                    color: "#FFFFFF"
                    border.width: 1
                    border.color: window.border
                    GridLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        columns: 2
                        rowSpacing: 8
                        Label {
                            text: "累计电量"
                            color: muted
                        }
                        Label {
                            text: (Number(mobileApi.flow.energyMwh || 0) / 1000000).toFixed(3) + " kWh"
                            color: ink
                            font.bold: true
                        }
                        Label {
                            text: "当前费用"
                            color: muted
                        }
                        Label {
                            text: window.money(mobileApi.flow.amountCent)
                            color: green
                            font.bold: true
                        }
                        Label {
                            text: "实时功率"
                            color: muted
                        }
                        Label {
                            text: (Number(mobileApi.flow.powerWatt || 0) / 1000).toFixed(1) + " kW"
                            color: ink
                            font.bold: true
                        }
                    }
                }
                Label {
                    text: mobileApi.flow.quote ? "当前报价 " + window.money(mobileApi.flow.quote.totalPriceCentPerKwh) + " / kWh · " + (mobileApi.flow.quote.chargerCode || "") : (mobileApi.flow.queuePosition ? "排队第 " + mobileApi.flow.queuePosition + " 位" : "")
                    color: muted
                    Layout.fillWidth: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        text: "确认报价"
                        Layout.fillWidth: true
                        enabled: !mobileApi.busy && Number(mobileApi.flow.status) === 20 && !!mobileApi.flow.quote
                        onClicked: mobileApi.confirmCharge()
                        background: Rectangle {
                            radius: 11
                            color: parent.enabled ? paleGreen : "#E9ECEA"
                        }
                        contentItem: Label {
                            text: parent.text
                            color: parent.enabled ? green : "#9AA49D"
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                    Button {
                        text: "开始充电"
                        Layout.fillWidth: true
                        enabled: !mobileApi.busy && Number(mobileApi.flow.status) === 30
                        onClicked: mobileApi.startCharge()
                        background: Rectangle {
                            radius: 11
                            color: parent.enabled ? green : "#C8D7CD"
                        }
                        contentItem: Label {
                            text: parent.text
                            color: "white"
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
                Button {
                    text: "结束充电并结算"
                    Layout.fillWidth: true
                    enabled: !mobileApi.busy && Number(mobileApi.flow.status) === 40
                    onClicked: mobileApi.settleCharge()
                    background: Rectangle {
                        radius: 11
                        color: parent.enabled ? "#0F9D71" : "#E1E8DA"
                    }
                    contentItem: Label {
                        text: parent.text
                        color: parent.enabled ? "white" : "#687762"
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
                Button {
                    text: "取消预约"
                    enabled: !mobileApi.busy
                    visible: Number(mobileApi.flow.status) === 10 || Number(mobileApi.flow.status) === 20 || Number(mobileApi.flow.status) === 30
                    Layout.fillWidth: true
                    onClicked: mobileApi.cancelCharge()
                    background: Rectangle {
                        radius: 11
                        color: "#FFF3C5"
                    }
                    contentItem: Label {
                        text: parent.text
                        color: "#886719"
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
                Label {
                    text: mobileApi.message
                    color: mobileApi.messageError ? "#B42318" : green
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Item {
                    Layout.fillHeight: true
                }
            }
        }

        Item {
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12
                Label {
                    text: "充电小票"
                    color: ink
                    font.pixelSize: 25
                    font.bold: true
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 270
                    radius: 14
                    color: "#FFFFFF"
                    border.width: 1
                    border.color: window.border
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 20
                        spacing: 9
                        Label {
                            text: mobileApi.receipt.stationName || "充电站"
                            color: ink
                            font.pixelSize: 17
                            font.bold: true
                        }
                        Label {
                            text: (mobileApi.receipt.chargerCode || "") + " · " + (mobileApi.receipt.orderNo || "")
                            color: muted
                            font.pixelSize: 12
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: "#EEF1EF"
                        }
                        Label {
                            text: "充电电量  " + (Number(mobileApi.receipt.energyMwh || 0) / 1000000).toFixed(3) + " kWh"
                            color: muted
                        }
                        Label {
                            text: "支付金额  " + window.money(mobileApi.receipt.paidCent !== undefined ? mobileApi.receipt.paidCent : mobileApi.receipt.amountCent)
                            color: green
                            font.pixelSize: 22
                            font.bold: true
                        }
                        Label {
                            text: "结算后余额  " + window.money(mobileApi.receipt.balanceAfterCent)
                            color: muted
                        }
                        Label {
                            text: mobileApi.receipt.statusText || "已完成"
                            color: green
                            font.bold: true
                        }
                    }
                }
                ActionButton {
                    text: "评价充电体验"
                    visible: mobileApi.receipt.statusText === "已完成"
                    Layout.fillWidth: true
                    onClicked: window.openReview(mobileApi.receipt.orderNo)
                }
                Button {
                    text: "完成，返回首页"
                    Layout.fillWidth: true
                    onClicked: window.go("home")
                    background: Rectangle {
                        radius: 12
                        color: green
                    }
                    contentItem: Label {
                        text: parent.text
                        color: "white"
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
                Item {
                    Layout.fillHeight: true
                }
            }
        }

        Item {
            id: navPage
            property bool navMapExpanded: false
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        text: "‹ 返回详情"
                        onClicked: window.page = "detail"
                        background: Rectangle {
                            color: "transparent"
                        }
                        contentItem: Label {
                            text: parent.text
                            color: green
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Button {
                        visible: navPage.navMapExpanded
                        text: "收起地图"
                        onClicked: navPage.navMapExpanded = false
                        background: Rectangle {
                            radius: 10
                            color: paleGreen
                        }
                        contentItem: Label {
                            text: parent.text
                            color: green
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
                Field {
                    id: originAddress
                    visible: !navPage.navMapExpanded
                    placeholderText: "导航起点地址（留空使用所选模拟位置）"
                    Layout.fillWidth: true
                }
                ActionButton {
                    visible: !navPage.navMapExpanded
                    text: "从此处规划"
                    enabled: !mobileApi.busy
                    Layout.fillWidth: true
                    onClicked: {
                        mobileApi.setLocation(regionBox.currentIndex, originAddress.text);
                        mobileApi.loadRoute(Number(selectedStation.id), "driving");
                    }
                }
                Label {
                    visible: !navPage.navMapExpanded
                    text: "一键导航"
                    color: ink
                    font.pixelSize: 23
                    font.bold: true
                }
                RowLayout {
                    visible: !navPage.navMapExpanded
                    Layout.fillWidth: true
                    Repeater {
                        model: [
                            {
                                label: "驾车",
                                value: "driving"
                            },
                            {
                                label: "步行",
                                value: "walking"
                            },
                            {
                                label: "公交",
                                value: "transit"
                            }
                        ]
                        Button {
                            text: modelData.label
                            Layout.fillWidth: true
                            onClicked: mobileApi.loadRoute(Number(selectedStation.id), modelData.value)
                            background: Rectangle {
                                radius: 10
                                color: mobileApi.route.mode === modelData.value ? green : paleGreen
                            }
                            contentItem: Label {
                                text: parent.text
                                color: mobileApi.route.mode === modelData.value ? "white" : green
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                    }
                }
                Rectangle {
                    visible: !navPage.navMapExpanded
                    Layout.fillWidth: true
                    Layout.preferredHeight: 115
                    radius: 14
                    color: "#FFFFFF"
                    border.width: 1
                    border.color: window.border
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 15
                        Label {
                            text: mobileApi.route.stationName || selectedStation.name || "正在规划路线…"
                            color: ink
                            font.bold: true
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label {
                            text: mobileApi.route.distanceMeter ? window.distance(mobileApi.route.distanceMeter) + " · " + (Number(mobileApi.route.durationSecond || 0) > 0 ? Math.ceil(Number(mobileApi.route.durationSecond) / 60) + " 分钟" : "预计时间未知") : (mobileApi.busy ? "正在请求服务端路线…" : "路线未就绪，请重试")
                            color: muted
                            font.pixelSize: 14
                        }
                        Label {
                            text: mobileApi.route.routeFallback ? "路线服务降级，已提供本地距离" : (mobileApi.route.provider || "")
                            color: mobileApi.route.routeFallback ? "#886719" : green
                            font.pixelSize: 12
                        }
                    }
                }
                RowLayout {
                    visible: !navPage.navMapExpanded && !!mobileApi.route.browserUrl
                    Layout.fillWidth: true
                    Label {
                        text: "路线地图"
                        color: muted
                        font.pixelSize: 12
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Button {
                        text: "⛶ 展开地图"
                        onClicked: navPage.navMapExpanded = true
                        background: Rectangle {
                            radius: 10
                            color: paleGreen
                        }
                        contentItem: Label {
                            text: parent.text
                            color: green
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
                Loader {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    active: window.page === "navigation" && !!mobileApi.route.browserUrl
                    sourceComponent: WebView {
                        url: mobileApi.route.browserUrl || "about:blank"
                    }
                }
                ListView {
                    visible: !navPage.navMapExpanded
                    Layout.fillWidth: true
                    Layout.preferredHeight: 80
                    clip: true
                    spacing: 6
                    model: mobileApi.route.steps || []
                    delegate: Label {
                        width: parent.width
                        text: "• " + (modelData.instruction || "") + "  " + window.distance(modelData.distanceMeter)
                        color: muted
                        wrapMode: Text.Wrap
                    }
                }
                Button {
                    visible: !navPage.navMapExpanded && !!mobileApi.route.browserUrl
                    text: "使用地图应用继续导航"
                    Layout.fillWidth: true
                    onClicked: Qt.openUrlExternally(mobileApi.route.browserUrl)
                    background: Rectangle {
                        radius: 11
                        color: paleGreen
                    }
                    contentItem: Label {
                        text: parent.text
                        color: green
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        anchors.bottomMargin: 76
        height: statusMessage.implicitHeight + 20
        radius: 12
        color: "#FFF0ED"
        visible: mobileApi.messageError && window.page !== "login" && window.page !== "profile" && window.page !== "charge"
        Label {
            id: statusMessage
            anchors.fill: parent
            anchors.margins: 10
            text: mobileApi.message
            color: "#B42318"
            wrapMode: Text.Wrap
        }
    }
    BusyIndicator {
        anchors.right: parent.right
        anchors.top: parent.top
        width: 36
        height: 36
        running: mobileApi.busy
        visible: running
    }
    Loader {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 70
        visible: window.page !== "login"
        sourceComponent: bottomBar
    }
    Component {
        id: bottomBar
        Rectangle {
            color: "#FFFFFF"
            border.color: "#E7EBE8"
            border.width: 1
            RowLayout {
                anchors.fill: parent
                anchors.margins: 7
                Repeater {
                    model: [
                        {
                            label: "首页",
                            value: "home"
                        },
                        {
                            label: "充电",
                            value: "charge"
                        },
                        {
                            label: "订单",
                            value: "orders"
                        },
                        {
                            label: "我的",
                            value: "profile"
                        }
                    ]
                    Button {
                        text: modelData.label
                        Layout.fillWidth: true
                        onClicked: window.go(modelData.value)
                        background: Rectangle {
                            radius: 12
                            color: window.page === modelData.value ? "#E7F5F1" : "transparent"
                        }
                        contentItem: Label {
                            text: parent.text
                            color: window.page === modelData.value ? green : "#7B828A"
                            font.bold: window.page === modelData.value
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }
        }
    }

    Sheet {
        id: serverDialog
        anchors.centerIn: parent
        modal: true
        title: "服务器设置"
        width: Math.min(window.width - 32, 390)
        standardButtons: Dialog.NoButton
        contentItem: ColumnLayout {
            Label {
                text: "输入服务地址，保存后重新登录"
                color: muted
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Field {
                id: serverField
                text: mobileApi.serverUrl
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhUrlCharactersOnly
            }
            ActionButton {
                text: "保存地址"
                Layout.fillWidth: true
                onClicked: if (mobileApi.configureServer(serverField.text))
                    serverDialog.close()
            }
            Label {
                text: mobileApi.message
                color: mobileApi.messageError ? "#B42318" : green
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }
    }
    Sheet {
        id: rechargeDialog
        anchors.centerIn: parent
        modal: true
        title: "余额充值"
        width: Math.min(window.width - 32, 390)
        standardButtons: Dialog.NoButton
        contentItem: ColumnLayout {
            Label {
                text: "虚拟支付 · 金额 0.01～10000 元"
                color: muted
            }
            Field {
                id: rechargeAmount
                placeholderText: "充值金额（元）"
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                Layout.fillWidth: true
            }
            ActionButton {
                text: "确认充值"
                enabled: !mobileApi.busy
                Layout.fillWidth: true
                onClicked: mobileApi.recharge(rechargeAmount.text)
            }
            Label {
                text: mobileApi.message
                color: mobileApi.messageError ? "#B42318" : green
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }
    }
    Sheet {
        id: deletionDialog
        anchors.centerIn: parent
        modal: true
        title: "确认注销账户"
        width: Math.min(window.width - 32, 390)
        standardButtons: Dialog.NoButton
        contentItem: ColumnLayout {
            Label {
                text: "注销将撤销全部登录会话并匿名化账户，此操作不可撤销。"
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                color: "#B42318"
            }
            ActionButton {
                text: "获取注销验证码"
                enabled: !mobileApi.busy
                Layout.fillWidth: true
                onClicked: mobileApi.requestDeletionCode()
            }
            Field {
                id: deletionCode
                placeholderText: "6 位验证码"
                maximumLength: 6
                inputMethodHints: Qt.ImhDigitsOnly
                Layout.fillWidth: true
            }
            ActionButton {
                text: "确认永久注销"
                enabled: !mobileApi.busy && deletionCode.text.length === 6
                Layout.fillWidth: true
                onClicked: mobileApi.deleteAccount(deletionCode.text)
            }
            Label {
                text: mobileApi.message
                color: mobileApi.messageError ? "#B42318" : green
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }
        Connections {
            target: mobileApi
            function onSessionChanged() {
                if (!mobileApi.loggedIn)
                    deletionDialog.close();
            }
        }
    }
    Sheet {
        id: avatarChoice
        anchors.centerIn: parent
        modal: true
        title: "更换头像"
        width: Math.min(window.width - 32, 390)
        standardButtons: Dialog.NoButton
        contentItem: ColumnLayout {
            ActionButton {
                text: "拍照"
                Layout.fillWidth: true
                onClicked: {
                    avatarChoice.close();
                    cameraDialog.open();
                }
            }
            ActionButton {
                text: "从本地选择"
                Layout.fillWidth: true
                onClicked: {
                    avatarChoice.close();
                    avatarFile.open();
                }
            }
        }
    }
    FileDialog {
        id: avatarFile
        title: "选择头像"
        nameFilters: ["图片 (*.png *.jpg *.jpeg *.bmp)"]
        onAccepted: mobileApi.uploadAvatar(selectedFile.toString())
    }
    Sheet {
        id: cameraDialog
        anchors.centerIn: parent
        modal: true
        title: "拍摄头像"
        standardButtons: Dialog.NoButton
        width: Math.min(window.width - 24, 390)
        height: Math.min(window.height - 40, 510)
        property string capturedFile: ""
        property string captureError: ""
        property bool capturing: false
        onOpened: {
            capturedFile = "";
            captureError = "";
            capturing = false;
            mobileApi.discardCapture();
            if (mobileApi.cameraPermissionGranted())
                camera.start();
            else
                mobileApi.requestCameraPermission();
        }
        onClosed: {
            camera.stop();
            mobileApi.discardCapture();
            capturedFile = "";
            capturing = false;
        }
        Connections {
            target: mobileApi
            function onCameraPermissionResult(granted) {
                if (!cameraDialog.visible)
                    return;
                if (granted)
                    camera.start();
                else
                    cameraDialog.captureError = "相机权限被拒绝，请在系统设置中允许使用相机";
            }
        }
        contentItem: ColumnLayout {
            spacing: 10
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                VideoOutput {
                    id: cameraOutput
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectFit
                    visible: !cameraDialog.capturedFile
                }
                Image {
                    anchors.fill: parent
                    source: cameraDialog.capturedFile ? "file://" + cameraDialog.capturedFile : ""
                    cache: false
                    fillMode: Image.PreserveAspectFit
                    visible: !!cameraDialog.capturedFile
                }
            }
            Label {
                text: cameraDialog.captureError || (cameraDialog.capturedFile ? "确认照片，或重拍" : !mediaDevices.videoInputs.length ? "未检测到摄像头" : imageCapture.readyForCapture ? "保持正脸并点击拍摄" : "相机正在准备…")
                color: cameraDialog.captureError ? "#B42318" : muted
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.fillWidth: true
                ActionButton {
                    text: "切换镜头"
                    visible: !cameraDialog.capturedFile
                    enabled: mediaDevices.videoInputs.length > 1 && !cameraDialog.capturing
                    Layout.fillWidth: true
                    onClicked: {
                        camera.stop();
                        cameraDialog.deviceIndex = (cameraDialog.deviceIndex + 1) % mediaDevices.videoInputs.length;
                        camera.start();
                    }
                }
                ActionButton {
                    text: cameraDialog.capturing ? "拍摄中…" : "拍摄"
                    visible: !cameraDialog.capturedFile
                    enabled: imageCapture.readyForCapture && !cameraDialog.capturing
                    Layout.fillWidth: true
                    onClicked: {
                        cameraDialog.capturing = true;
                        imageCapture.captureToFile(mobileApi.avatarCapturePath());
                    }
                }
                ActionButton {
                    text: "重拍"
                    visible: !!cameraDialog.capturedFile
                    Layout.fillWidth: true
                    onClicked: {
                        cameraDialog.capturedFile = "";
                        mobileApi.discardCapture();
                        camera.start();
                    }
                }
                ActionButton {
                    text: "确认上传"
                    visible: !!cameraDialog.capturedFile
                    enabled: !mobileApi.busy
                    Layout.fillWidth: true
                    onClicked: {
                        mobileApi.uploadAvatar(cameraDialog.capturedFile);
                        cameraDialog.close();
                    }
                }
            }
        }
        property int deviceIndex: 0
    }
    Sheet {
        id: reviewDialog
        anchors.centerIn: parent
        modal: true
        title: "评价充电体验"
        standardButtons: Dialog.NoButton
        width: Math.min(window.width - 24, 390)
        property int rating: 0
        onAboutToShow: {
            rating = mobileApi.reviewInfo.existing ? (mobileApi.reviewInfo.rating || 0) : 0;
            reviewContent.text = mobileApi.reviewInfo.content || "";
        }
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                text: mobileApi.reviewInfo.orderNo || ""
                color: "#8A9A90"
                font.pixelSize: 11
                Layout.fillWidth: true
            }
            Row {
                spacing: 3
                Repeater {
                    model: 5
                    Button {
                        flat: true
                        enabled: !mobileApi.reviewInfo.existing && !mobileApi.reviewInfo.busy
                        onClicked: reviewDialog.rating = index + 1
                        contentItem: Label {
                            text: "★"
                            font.pixelSize: 30
                            color: index < reviewDialog.rating ? green : "#C9D6CE"
                        }
                        background: Rectangle {
                            color: "transparent"
                        }
                    }
                }
            }
            Label {
                text: mobileApi.reviewInfo.submitted ? "评价已提交，感谢你的反馈！" : mobileApi.reviewInfo.existing ? "该订单已评价，评价内容不可修改" : "点击星星选择评分"
                color: muted
                font.pixelSize: 12
            }
            TextArea {
                id: reviewContent
                Layout.fillWidth: true
                Layout.preferredHeight: 110
                placeholderText: "分享你的充电体验（1～500 字）"
                wrapMode: TextArea.Wrap
                text: mobileApi.reviewInfo.content || ""
                readOnly: !!(mobileApi.reviewInfo.existing || mobileApi.reviewInfo.busy)
                onTextChanged: if (length > 500)
                    remove(500, length)
                background: Rectangle {
                    radius: 10
                    color: "white"
                    border.color: window.border
                }
            }
            Label {
                text: reviewContent.length + "/500"
                color: "#8A9A90"
                font.pixelSize: 11
                Layout.alignment: Qt.AlignRight
            }
            Label {
                text: mobileApi.reviewInfo.error || ""
                color: "#B42318"
                wrapMode: Text.Wrap
                visible: text.length > 0
                Layout.fillWidth: true
            }
            Button {
                text: mobileApi.reviewInfo.busy ? "提交中…" : "提交评价"
                visible: !mobileApi.reviewInfo.existing
                enabled: !mobileApi.reviewInfo.busy && reviewDialog.rating > 0 && reviewContent.text.trim().length > 0
                Layout.fillWidth: true
                onClicked: mobileApi.submitReview(reviewDialog.rating, reviewContent.text)
                background: Rectangle {
                    radius: 11
                    color: parent.enabled ? green : "#C8D7CD"
                }
                contentItem: Label {
                    text: parent.text
                    color: "white"
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }
    }
    MediaDevices {
        id: mediaDevices
    }
    Camera {
        id: camera
        cameraDevice: mediaDevices.videoInputs.length ? mediaDevices.videoInputs[cameraDialog.deviceIndex] : mediaDevices.defaultVideoInput
        onErrorOccurred: (error, errorString) => {
            cameraDialog.captureError = "相机不可用，请关闭其他相机应用后重试";
            cameraDialog.capturing = false;
        }
    }
    CaptureSession {
        id: captureSession
        camera: camera
        videoOutput: cameraOutput
        imageCapture: ImageCapture {
            id: imageCapture
            onErrorOccurred: (id, error, errorString) => {
                cameraDialog.captureError = "拍摄失败，请重试";
                cameraDialog.capturing = false;
            }
            onImageSaved: (id, fileName) => {
                cameraDialog.capturing = false;
                if (!cameraDialog.visible) {
                    mobileApi.discardCapture();
                    return;
                }
                cameraDialog.capturedFile = fileName;
                camera.stop();
            }
        }
    }
}
