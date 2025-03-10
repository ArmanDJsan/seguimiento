import os 
import cv2
import numpy as np
from time import sleep

os.system('cls')

# Define the range for the colors in HSV
color_ranges = {
    'yellow': ([20, 100, 100], [30, 255, 255]),
    'blue': ([100, 150, 50], [140, 255, 255]),
    'red': ([0, 70, 50], [10, 255, 255]),  # Range for red color
    'green': ([35, 100, 100], [85, 255, 255]),  # Range for green color
    'orange': ([10, 100, 20], [25, 255, 255])  # Range for orange color
}

muestra = cv2.VideoCapture('circles.mp4')
estado = True

def find_object(imagen, mask, color):
    cnts, hieredachy = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if cnts:
        c = max(cnts, key=cv2.contourArea)
        x, y, w, h = cv2.boundingRect(c)
        cv2.rectangle(imagen, (x+1, y+1), (x+w+1, y+h+1), color, 2)
        return (round(x+w/2), round(y+h/2))
    return None

def detect_red_dotted_line(image):
    hsv = cv2.cvtColor(image, cv2.COLOR_BGR2HSV)
    lower_red = np.array([0, 70, 50])
    upper_red = np.array([10, 255, 255])
    mask = cv2.inRange(hsv, lower_red, upper_red)
    edges = cv2.Canny(mask, 50, 150, apertureSize=3)
    lines = cv2.HoughLinesP(edges, 1, np.pi/180, threshold=100, minLineLength=100, maxLineGap=10)
    
    if lines is not None:
        for line in lines:
            x1, y1, x2, y2 = line[0]
            if x1 == x2:  # Check if the line is vertical
                return x1
    return None

crossed_objects = set()
number=0
while estado:
    estado, fotograma = muestra.read()
    
    if not estado or fotograma is None:
        break
    
    if cv2.waitKey(1) == 103:
        break
    
    # Detect the red dotted line
    line_position = detect_red_dotted_line(fotograma)
   
    if line_position is not None:
        hsv = cv2.cvtColor(fotograma, cv2.COLOR_BGR2HSV)
        for color_name, (lower, upper) in color_ranges.items():
            lower_np = np.array(lower)
            upper_np = np.array(upper)
            mask = cv2.inRange(hsv, lower_np, upper_np)
            py = find_object(fotograma, mask, (0, 255, 255) if color_name == 'yellow' else (255, 0, 0))
            if py:
                if py[0] > line_position and color_name not in crossed_objects and color_name not in "red":
                    print(f"{color_name} object crossed the red line #{number+1}" )
                    crossed_objects.add(color_name)
                    number=number+1
    
    cv2.imshow('Camara Web', fotograma)
    
print("Fin del programa")