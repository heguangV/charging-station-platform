// Dashboard 快照原子写入器：先把内容写入临时文件，再通过原子 rename 替换目标文件
// （dashboard.json，由 Dashboard 后台任务每 30 秒导出一次），读取端不会看到半成品。
// 写入失败时保留上一份成功的快照文件。
#pragma once

#include <string>

namespace ncs::infrastructure::files
{

class AtomicSnapshotWriter final
{
  public:
    explicit AtomicSnapshotWriter(std::string destinationPath)
        : destinationPath_(std::move(destinationPath))
    {
    }
    bool write(const std::string& contents) const;
    const std::string& destinationPath() const
    {
        return destinationPath_;
    }

  private:
    std::string destinationPath_;
};

} // namespace ncs::infrastructure::files
