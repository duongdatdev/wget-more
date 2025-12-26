# Chi tiết Hiện thực (Implementation Details)

## Bảng Tổng hợp các File đã Chỉnh sửa

Để trả lời câu hỏi "những file nào đã được sửa", dưới đây là danh sách chi tiết:

| File | Chức năng chính | Mô tả thay đổi chính |
| :--- | :--- | :--- |
| `src/tui.c` | **TUI, Checksum** | **[NEW]** File mới hoàn toàn. Chứa toàn bộ logic hiển thị giao diện ncurses và tính toán/verify checksum. |
| `src/tui.h` | **TUI, Checksum** | **[NEW]** Header file cho TUI, định nghĩa các hàm public và struct. |
| `src/main.c` | **Multithread** | Sửa hàm `main`: Khởi tạo TUI, tạo Thread Pool (`pthread_create`), điều phối tải song song. Chứa hàm `tui_download_thread`. |
| `src/retr.c` | **TUI Integration** | Sửa hàm `fd_read_body`: Thêm logic kiểm tra `tui_is_paused()` để tạm dừng tải, và `tui_is_cancelled()` để hủy. Cập nhật tiến trình cho TUI. |
| `src/Makefile.am` | **Build System** | Thêm `tui.c` và `tui.h` vào danh sách `wget_SOURCES` để biên dịch cùng dự án. Thêm `-lncurses` vào `LDADD`. |
| `src/wget.h` | **Global Include** | Thêm `#include "tui.h"` (hoặc định nghĩa liên quan) để các file khác có thể gọi hàm TUI. |

---

Tài liệu này mô tả chi tiết cách các tính năng **TUI**, **Multithread Download**, và **Checksum** được hiện thực trong dự án hiện tại (dựa trên source code `wget` đã chỉnh sửa).

## 1. TUI (Text User Interface)

Giao diện người dùng dạng văn bản (TUI) được xây dựng dựa trên thư viện **ncurses**, cho phép hiển thị trực quan tiến trình tải xuống và tương tác người dùng.

### Cấu trúc và File chính
*   **File nguồn**: `src/tui.c` và `src/tui.h`.
*   **Thư viện**: Sử dụng `ncurses` (`#include <ncurses.h>`).

### Các thành phần cốt lõi
1.  **Khởi tạo (Initialization)**:
    *   Hàm `init_ncurses_base()`: Thiết lập môi trường ncurses (khởi tạo màn hình `initscr()`, kích hoạt màu `start_color()`, tắt hiển thị con trỏ `curs_set(0)`, v.v.).
    *   Hàm `ensure_main_win()`: Tạo cửa sổ chính (`main_win`) để vẽ khung và tiêu đề.

2.  **Giao diện nhập liệu (Batch Downloader)**:
    *   Hàm `tui_get_info()`: Hiển thị giao diện ban đầu cho phép người dùng:
        *   Nhập danh sách URL cần tải.
        *   Thêm (`a`) hoặc xóa (`d`) URL.
        *   Bắt đầu tải (`s`) hoặc thoát (`q`).
    *   Sử dụng vòng lặp `while(1)` để bắt sự kiện phím và cập nhật màn hình nhập liệu.

3.  **Hiển thị tiến trình (Progress Drawing)**:
    *   Hàm `tui_progress_draw(void *bar_ptr)`: Chịu trách nhiệm vẽ lại toàn bộ danh sách các thanh tiến trình trên `main_win`.
    *   Duyệt qua danh sách `bars` (mảng các `TuiProgress`).
    *   Với mỗi file đang tải, nó hiển thị:
        *   Tên file.
        *   Thanh loading màu xanh (sử dụng ký tự `ACS_CKBOARD` hoặc khoảng trắng).
        *   Phần trăm hoàn thành, tốc độ (KB/s), và thời gian dự kiến (ETA).
    *   Sử dụng các cặp màu (`COLOR_PAIR`) để làm nổi bật (Ví dụ: màu vàng cho trạng thái Pause, xanh cho đang chạy).

4.  **Xử lý sự kiện (Input Handling)**:
    *   Chạy trên một luồng riêng biệt: `tui_input_handler` (được tạo bởi `pthread_create`).
    *   Vòng lặp liên tục kiểm tra phím bấm (`wgetch` với `nodelay`).
    *   Chức năng:
        *   `p` / `P`: Tạm dừng (Pause) - thiết lập biến toàn cục `tui_paused`.
        *   `c` / `C` / `ESC`: Hủy (Cancel) - thiết lập biến `tui_cancelled`.

5.  **Tích hợp với Logic tải xuống**:
    *   Trong `src/retr.c`, hàm `fd_read_body` (nơi đọc dữ liệu từ socket) liên tục kiểm tra trạng thái TUI. Nếu `tui_is_paused()` trả về true, nó sẽ ngủ (`usleep`) thay vì đọc dữ liệu.

## 2. Multithread Download (Tải đa luồng)

Cơ chế đa luồng hiện tại được thiết kế theo mô hình **Parallel Files** (Tải song song nhiều file), không phải là Multipart (chia nhỏ một file thành nhiều phần).

### Cấu trúc và Logic
*   **File nguồn**: Logic điều phối nằm trong `src/main.c`.
*   **Thread Function**: `tui_download_thread(void *arg)` (trong `src/main.c`).

### Cách hoạt động
1.  **Mô hình Thread Pool đơn giản**:
    *   Dựa trên tham số `opt.connections` (số lượng kết nối/luồng tối đa).
    *   Chương trình tạo một mảng `pthread_t` để quản lý các luồng.

2.  **Quy trình trong `main.c`**:
    *   Lấy danh sách URL từ TUI (`tui_get_info`).
    *   Khởi tạo `struct tui_download_ctx` cho mỗi URL (chứa URL và trạng thái kết quả).
    *   **Vòng lặp điều phối**:
        1.  Lấp đầy các slot luồng trống bằng cách gọi `pthread_create` với hàm thực thi là `tui_download_thread`.
        2.  Mỗi luồng sẽ đảm nhận việc tải **một URL trọn vẹn**.
        3.  Khi đạt giới hạn số luồng (`max_parallel`), chương trình chờ (`pthread_join`) cho đến khi *bất kỳ* luồng nào hoàn thành.
        4.  Khi một luồng xong, slot đó được tái sử dụng cho URL tiếp theo trong danh sách chờ.

3.  **Hàm `tui_download_thread`**:
    *   Nhận thông tin URL.
    *   Gọi hàm `retrieve_url` (hàm tải xuống tiêu chuẩn của wget) một cách đồng bộ.
    *   Do `retrieve_url` chạy trong thread riêng, việc blocking (chờ I/O) của nó không ảnh hưởng đến các luồng khác hay luồng chính.

## 3. Checksum (Kiểm tra toàn vẹn)

Tính năng Checksum được tích hợp trực tiếp vào module TUI sử dụng thư viện **Nettle**.

### Cấu trúc và Logic
*   **File nguồn**: `src/tui.c`.
*   **Thư viện**: `nettle/md5.h`, `nettle/sha2.h` (được bảo vệ bởi `#ifdef HAVE_NETTLE`).

### Cách hoạt động
1.  **Dữ liệu Checksum**:
    *   Struct `TuiProgress` có chứa trường `checksum_type`, `checksum` (kết quả tính toán) và `expected_checksum` (kỳ vọng).

2.  **Tính toán (Calculation)**:
    *   Hàm `calculate_md5` và `calculate_sha256`:
        *   Mở file đã tải trên đĩa (`fopen` chế độ `rb`).
        *   Đọc file theo từng chunk (8KB).
        *   Cập nhật băm (hash context) sử dụng các hàm của Nettle (`md5_update`, `sha256_update`).
        *   Tạo chuỗi hex digest cuối cùng.

3.  **Quy trình kích hoạt**:
    *   Khi một file tải xong, hàm `tui_progress_finish_with_checksum` được gọi.
    *   Hàm này tự động gọi `calculate_checksum` nếu file path hợp lệ.
    *   Sau đó gọi `verify_checksum` để so sánh chuỗi hash tính được với `expected_checksum` (so sánh không phân biệt hoa thường).

4.  **Hiển thị kết quả**:
    *   Kết quả (`checksum_verified`) được lưu vào struct và được `tui_progress_draw` hiển thị lên màn hình (mặc dù đoạn code vẽ cụ thể chưa thấy rõ trong các dòng đã đọc, nhưng logic tính toán đã hoàn tất).
