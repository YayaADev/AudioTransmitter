**Wireless S/PDIF Digital Audio Transmitter with IR Control**

Streams raw PCm audio over wifi to an ESP32 that streams it as S/PDIF to my sony soundbar via a Toslink cable. Uses a ring buffer strategy for network jitter and smoother playback.

The board is ona  breadbopard with one resitoor + LED combo for the audio output and one resitor + LED combo for an IR blaster (to turn my soundbar on remotely)

Plans I have are to setup a script on my SBC and have it play audio in scheduled times. 


Get a sample mp3 to test here : https://pixabay.com/music/search/sample/
Run this command to test
`ffmpeg -re -i sample.mp3 -acodec pcm_s16le -ar 44100 -ac 2 -f s16le udp://esp_ip:1234`