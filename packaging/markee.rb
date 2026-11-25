class Markee < Formula
  desc "Desktop 72x16 24-bit RGB LED matrix display simulator with HTTP API"
  homepage "https://github.com/stevenspencer/markee"
  url "https://github.com/stevenspencer/markee/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "REPLACE_WITH_RELEASE_TARBALL_SHA256"
  license "MIT"
  head "https://github.com/stevenspencer/markee.git", branch: "master"

  depends_on "pkg-config" => :build
  depends_on "raylib"

  def install
    system "make", "release"
    bin.install "markee"
    doc.install "README.md", "docs/API.md"
    (pkgshare/"examples").install Dir["examples/*"]
  end

  service do
    run [opt_bin/"markee", "--always-on-top", "--borderless", "--corner", "top-right", "--save-position"]
    keep_alive false
    log_path var/"log/markee.log"
    error_log_path var/"log/markee.log"
  end

  test do
    assert_match "Usage:", shell_output("#{bin}/markee --help")
  end
end
