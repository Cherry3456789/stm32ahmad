import tkinter as tk
from PIL import Image, ImageDraw
import numpy as np

def get_drawing_buffer():
    # Configuration
    size, scale = 28, 10
    canvas_dim = size * scale
    result = {"data": None}

    root = tk.Tk()
    root.title("Draw then Close")

    cv = tk.Canvas(root, width=canvas_dim, height=canvas_dim, bg='black')
    cv.pack()
    img = Image.new("L", (canvas_dim, canvas_dim), 0)
    draw = ImageDraw.Draw(img)

    def paint(event):
        x, y, r = event.x, event.y, 8
        cv.create_oval(x-r, y-r, x+r, y+r, fill="white", outline="white")
        draw.ellipse([x-r, y-r, x+r, y+r], fill=255)

    def on_close():
        low_res = img.resize((size, size), Image.Resampling.LANCZOS)
        result["data"] = np.array(low_res)
        root.destroy()

    cv.bind("<B1-Motion>", paint)
    root.protocol("WM_DELETE_WINDOW", on_close)
    root.mainloop()
    
    return result["data"]
