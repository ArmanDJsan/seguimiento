import os 
import cv2
import numpy as np
from time import sleep

os.system('cls')

# Define the range for the colors in HSV
color_ranges = {
    'yellow': ([20, 100, 100], [30, 255, 255]),
    'blue': ([100, 150, 50], [140, 255, 255]) 
}

muestra = cv2.VideoCapture(0)
estado = True

# Variables to track which color reaches each line first
first_line_reached = {'yellow': False, 'blue': False}
middle_line_reached = {'yellow': False, 'blue': False}
last_line_reached = {'yellow': False, 'blue': False}

def find_object(imagen, mask, color):
    cnts, hieredachy = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if cnts:
        c = max(cnts, key=cv2.contourArea)
        x, y, w, h = cv2.boundingRect(c)
        cv2.rectangle(imagen, (x, y), (x+w, y+h), color, 2)
        return (round(x+w/2), round(y+h/2))
    return None

while estado:
    estado, fotograma = muestra.read()
    
    if cv2.waitKey(1) == 103:
        break
    
    hsv = cv2.cvtColor(fotograma, cv2.COLOR_BGR2HSV)
    
    for color_name, (lower, upper) in color_ranges.items():
        lower_np = np.array(lower)
        upper_np = np.array(upper)
        mask = cv2.inRange(hsv, lower_np, upper_np)
        py = find_object(fotograma, mask, (0, 255, 255) if color_name == 'yellow' else (255, 0, 0))
      
    
    # Draw the vertical lines
    height, width, _ = fotograma.shape
    first_line_x = int(width * 0.25)
    middle_line_x = int(width * 0.5)
    last_line_x = int(width * 0.75)
    cv2.line(fotograma, (first_line_x, 0), (first_line_x, height), (255, 255, 255), 2)
    cv2.line(fotograma, (middle_line_x, 0), (middle_line_x, height), (255, 255, 255), 2)
    cv2.line(fotograma, (last_line_x, 0), (last_line_x, height), (255, 255, 255), 2)
    
    # Check if the elements have crossed the lines
    if py:
        x, y = py
        if x >= first_line_x and not first_line_reached[color_name]:
            first_line_reached[color_name] = True
            print(f"{color_name.capitalize()} reached the first line first")
        if x >= middle_line_x and not middle_line_reached[color_name]:
            middle_line_reached[color_name] = True
            print(f"{color_name.capitalize()} reached the middle line first")
        if x >= last_line_x and not last_line_reached[color_name]:
            last_line_reached[color_name] = True
            print(f"{color_name.capitalize()} reached the last line first")
    
    cv2.imshow('Camara Web', fotograma)
    
    sleep(0.03)
    
print("Fin del programa")