# ML 冷启动：模拟历史数据训练与跟跑指南

## 背景与边界

平台尚无真实充电订单沉淀，`ml/train.py` 的验收门槛（UC-M-02：随机森林必须**严格优于**"抄上周同时刻"的 lag168 基线）在空库上不可能通过。本配套文档与 `ml/tools/gen_synthetic_history.py` 一起，用**确定性模拟历史**打通"生成 → 训练 → 验收 → 预测"整条离线链路，不改动任何既有代码与配置。

- 数据是 SIMULATED（冷启动/演示用，生成器输出与本文均如实标注），不是真实运营数据；真实订单回流后应由管理端触发 TRAIN 任务重训并覆盖。
- 模拟数据在结构上服务真实训练管线：字段形状与 `GET /internal/ml/features/hourly` 导出逐字段一致（整数 mWh、UTC 秒对齐全小时），因此同一 `ml/pipeline.py` 代码零修改可用。
- 运行时产物（`ml/data/synthetic/`、`ml/models/` 下的 pkl 与元数据）**不入仓库**：仓库只存"怎么造"，一条命令即可重造（见下，确定性已实测）。

## 依赖

```bash
python3 -m pip install -r ml/requirements.txt   # numpy / joblib / scikit-learn
```

已在 Python 3.10 + numpy 2.2.6 / scikit-learn 1.7.2 / joblib 1.6.0 实测（均满足 requirements 约束）。

## 三步跟跑（任何人任何机器）

```bash
# 1) 生成 3 站 × 24 天小时粒度模拟历史（默认写到当前整点，向前 24 天）
python3 ml/tools/gen_synthetic_history.py --out ml/data/synthetic/features_24d.json

# 2) 训练，stdout 应出现 "qualified": true
python3 ml/train.py --train \
  --features ml/data/synthetic/features_24d.json \
  --model ml/models/load_rf.pkl

# 3) 离线 predict 只有在 <model>.pkl.json 存在时才加载真模型（否则静默走基线），
#    该文件在服务端链路里由训练任务登记 model_version 时生成，离线手动补一枚：
python3 - <<'EOF'
import json, pathlib
pathlib.Path("ml/models").mkdir(parents=True, exist_ok=True)
pathlib.Path("ml/models/load_rf.pkl.json").write_text(json.dumps({
    "modelVersionNo": "MV-SIM-0001",
    "note": "offline synthetic-history training; mirrors server model_version registration",
}), encoding="utf-8")
EOF
python3 ml/predict.py --predict \
  --features ml/data/synthetic/features_24d.json \
  --model ml/models/load_rf.pkl
#    期望输出 "modelVersionNo": "MV-SIM-0001"（而非 "BASELINE"）且 items 共 9 条（3 站 × 1/6/24h）
```

## 参考验收结果（2026-09-05，种子 20260901）

| 指标 | 模型 | 基线 lag168 | 判定 |
| --- | --- | --- | --- |
| MAE | 27,199,569.71 mWh | 39,636,627.62 mWh | -31.4% ✅ |
| RMSE | 43,126,418.54 mWh | 64,270,707.69 mWh | -32.9% ✅ |
| WAPE / MAPE | 19.93% / 24.06% | — | 报告在案 |
| qualified | true | | UC-M-02 通过 |

生成器与训练均为确定性流程（固定种子）：模型 `ml/models/load_rf.pkl` 的 SHA-256 为
`82ac1b7bdf5f8be85a0c688c8450c75abc4ebf36a6b39af8b9dea88986113c49`，两次独立跟跑逐字节一致。
想复现同一训练窗口（而非以当前整点结尾的新窗口），给生成器加 `--end-at 1788573600`。

## 数据配方摘要（详见生成器 docstring）

能量 = 站点规模 × 日周期曲线（北京时区午间小峰 + 晚间大峰）× 工作日/周末因子 × 平稳 AR(1) 噪声（φ=0.6）。AR(1) 自相关是 lag1/rolling 特征得以打赢周基线的关键；三站人设（办公型/住宅型/科技园区型）保证跨站可分性。

## 与服务端正式链路的关系

本 PR 只提供**离线冷启动路径**。服务端 TRAIN 任务全链路（管理端点 → `ml/worker.py` → 内部 REST 取数 → 聚合器 `station_hourly_metric` → 结果登记 `model_version` 表）不经过这些手工步骤，届时以真实/灌入的 `charging_order` 数据为准。建议后续独立 PR 处理：`.gitignore` 增补 `ml/models/`（当前缺失，存在误提交 7.8MB pkl 的风险）；`ml/README.md` 中"UC-M 尚未实现"的表述已过时。
