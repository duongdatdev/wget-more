# Linux & Open Source Project - Wget TUI

Đây là phiên bản tùy chỉnh của **GNU Wget 1.25.0**, được tích hợp giao diện người dùng dạng văn bản (TUI - Text User Interface) sử dụng thư viện **Ncurses**. Dự án này cung cấp trải nghiệm tải xuống trực quan hơn với khả năng quản lý nhiều file và kiểm tra tính toàn vẹn dữ liệu.

## 🚀 Tính năng nổi bật

*   **Giao diện TUI trực quan**: Giao diện đồ họa trên nền terminal, dễ sử dụng hơn so với dòng lệnh truyền thống.
*   **Tải xuống hàng loạt (Batch Download)**: Hỗ trợ nhập và quản lý danh sách nhiều URL để tải xuống lần lượt.
*   **Hiển thị tiến trình sinh động**: Thanh tiến trình (Progress bar) với màu sắc, hiển thị tốc độ và phần trăm hoàn thành theo thời gian thực.
*   **Kiểm tra Checksum**: Tích hợp tính năng xác thực mã băm **SHA256** để đảm bảo tính toàn vẹn của file sau khi tải về.

## 🛠 Yêu cầu hệ thống

Để biên dịch và chạy dự án, bạn cần cài đặt các công cụ và thư viện sau trên Linux:

*   **Trình biên dịch**: GCC
*   **Công cụ build**: Make
*   **Thư viện Ncurses**: `libncurses-dev` (Debian/Ubuntu) hoặc `ncurses-devel` (CentOS/Fedora)
*   **Thư viện OpenSSL**: `libssl-dev` hoặc `openssl-devel`
*   **GnuTLS** (Tùy chọn, thường mặc định của Wget): `libgnutls28-dev`

## ⚙️ Hướng dẫn cài đặt và biên dịch

1.  **Cài đặt các thư viện phụ thuộc** (Ví dụ trên Ubuntu/Debian):
    ```bash
    sudo apt-get update
    sudo apt-get install build-essential libncurses-dev libssl-dev pkg-config libgnutls28-dev
    ```

2.  **Cấu hình dự án**:
    Tại thư mục gốc của dự án, chạy lệnh:
    ```bash
    ./configure
    ```

3.  **Biên dịch mã nguồn**:
    ```bash
    make
    ```

## 📖 Hướng dẫn sử dụng

Sau khi biên dịch thành công, file thực thi chính sẽ nằm trong thư mục `src/`.

1.  **Khởi chạy ứng dụng**:
    ```bash
    ./src/wget
    ```
    Chương trình sẽ khởi động giao diện TUI.

2.  **Thao tác trên giao diện**:
    *   **Nhập URL**: Điền đường dẫn file cần tải.
    *   **Nhập Checksum (Tùy chọn)**: Điền mã SHA256 để kiểm tra file sau khi tải.
    *   **Add**: Thêm URL vào danh sách chờ tải.
    *   **Download**: Bắt đầu tải xuống tất cả các file trong danh sách.
    *   **Điều hướng**: Sử dụng phím `Tab` hoặc các phím mũi tên để di chuyển giữa các trường nhập liệu và nút bấm. `Enter` để chọn.

## 📂 Cấu trúc thư mục chính

*   `src/`: Chứa mã nguồn chính của chương trình (bao gồm `main.c`, `tui.c`...).
*   `src/tui.c`: Mã nguồn xử lý giao diện Ncurses.
*   `configure`: Script cấu hình hệ thống build.
*   `Makefile`: File chỉ dẫn biên dịch (được tạo ra sau khi chạy configure).

---
*Dự án được phát triển dựa trên mã nguồn mở GNU Wget.*
