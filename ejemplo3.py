import os 
import cv2
import numpy as np
from time import sleep

os.system('cls')

# Define the range for the colors in HSV
color_ranges = {
    'yellow': ([20, 100, 100], [30, 255, 255]),
    'blue': ([100, 150, 0], [140, 255, 255]),
    'orange': ([10, 100, 20], [25, 255, 255]),
    'green': ([35, 100, 100], [85, 255, 255]),  # Adjusted to apple green
    'red': ([0, 120, 70], [10, 255, 255])
}

muestra = cv2.VideoCapture(0)
estado = True

# Initial positions and angles for the elements
angle_yellow = 0
angle_blue = 180
radius = 50
center_x, center_y = 320, 240  # Center of the frame

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
        py = find_object(fotograma, mask, (0, 255, 255))
       
    
    # Draw the vertical lines
    height, width, _ = fotograma.shape
    first_line_x = int(width * 0.25)
    middle_line_x = int(width * 0.5)
    last_line_x = int(width * 0.75)
    cv2.line(fotograma, (first_line_x, 0), (first_line_x, height), (255, 255, 255), 2)
    cv2.line(fotograma, (middle_line_x, 0), (middle_line_x, height), (255, 255, 255), 2)
    cv2.line(fotograma, (last_line_x, 0), (last_line_x, height), (255, 255, 255), 2)
    
    # Calculate new positions for the yellow and blue elements
    yellow_x = int(center_x + radius * np.cos(np.radians(angle_yellow)))
    yellow_y = int(center_y + radius * np.sin(np.radians(angle_yellow)))
    blue_x = int(center_x + radius * np.cos(np.radians(angle_blue)))
    blue_y = int(center_y + radius * np.sin(np.radians(angle_blue)))
    
    # Draw the yellow and blue elements
    cv2.circle(fotograma, (yellow_x, yellow_y), 10, (0, 255, 255), -1)  # Yellow circle
    cv2.circle(fotograma, (blue_x, blue_y), 10, (255, 0, 0), -1)  # Blue circle
    
    # Check if the elements have crossed the lines
    if yellow_x >= first_line_x and not first_line_reached['yellow']:
        first_line_reached['yellow'] = True
        print("Yellow reached the first line first")
    if blue_x >= first_line_x and not first_line_reached['blue']:
        first_line_reached['blue'] = True
        print("Blue reached the first line first")
    
    if yellow_x >= middle_line_x and not middle_line_reached['yellow']:
        middle_line_reached['yellow'] = True
        print("Yellow reached the middle line first")
    if blue_x >= middle_line_x and not middle_line_reached['blue']:
        middle_line_reached['blue'] = True
        print("Blue reached the middle line first")
    
    if yellow_x >= last_line_x and not last_line_reached['yellow']:
        last_line_reached['yellow'] = True
        print("Yellow reached the last line first")
    if blue_x >= last_line_x and not last_line_reached['blue']:
        last_line_reached['blue'] = True
        print("Blue reached the last line first")
    
    # Update the angles for the next frame
    angle_yellow = (angle_yellow + 5) % 360
    angle_blue = (angle_blue + 5) % 360
    
    cv2.imshow('Camara Web', fotograma)
    
    sleep(0.03)
    
print("Fin del programa")