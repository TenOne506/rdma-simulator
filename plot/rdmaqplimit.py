import matplotlib.pyplot as plt
import numpy as np

# 生成模拟数据（与您描述完全一致）
qp_counts = np.array([22, 44, 88, 176, 352, 704, 1408, 2816])
rc_ops = np.array([5.0, 12.5, 25.0, 32.5, 35.0, 30.0, 18.0, 8.0])  # RC先升后降
ud_ops = np.array([10.0, 12.5, 15.0, 17.5, 20.0, 22.5, 24.0, 25.0])  # UD缓慢上升

# 创建画布
plt.style.use('default')
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

# ---- 左图：RDMA Read (RC) ----
ax1.plot(qp_counts, rc_ops, 'r-', linewidth=2)
ax1.set_xscale('log')
ax1.set_xlabel("# QPs", fontsize=11)
ax1.set_ylabel("Million ops/sec", fontsize=11)
ax1.set_title("(a) RDMA read (RC)", fontsize=12)
ax1.set_ylim(0, 40)
ax1.grid(True, linestyle='--', alpha=0.3)

# ---- 右图：RDMA RPC (UD) ----
ax2.plot(qp_counts, ud_ops, 'g--x', markersize=8, linewidth=2)
ax2.set_xscale('log')
ax2.set_xlabel("# senders", fontsize=11)
ax2.set_title("(b) RDMA RPC (UD)", fontsize=12)
ax2.set_ylim(0, 40)
ax2.grid(True, linestyle='--', alpha=0.3)

plt.tight_layout()
plt.savefig("rdma_performance_recreated.png", dpi=300)
plt.show()

